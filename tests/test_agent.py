"""Hermetic process/API regressions: generated ROMs, no network or X16 ROM download."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'api' / 'python'))
from x16agent import X16Agent, AgentError

EMULATOR = sys.argv.pop(1)


def rom_image():
    rom = bytearray([0xea] * (32 * 16384))
    rom[0:3] = bytes.fromhex('4c00c0')  # JMP $c000
    rom[0x100:0x104] = bytes.fromhex('ee000440')  # IRQ: INC $0400; RTI
    rom[0x3ffa:0x4000] = struct.pack('<HHH', 0xc100, 0xc000, 0xc100)
    return rom


def png_pixels(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, chunks = 8, bytearray()
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos+4])[0]
        kind = data[pos+4:pos+8]
        body = data[pos+8:pos+8+length]
        crc = struct.unpack('>I', data[pos+8+length:pos+12+length])[0]
        assert crc == zlib.crc32(kind + body) & 0xffffffff
        if kind == b'IHDR':
            width, height, depth, color, comp, filt, interlace = struct.unpack('>IIBBBBB', body)
            assert (width, height, depth, color, comp, filt, interlace) == (640, 480, 8, 2, 0, 0, 0)
        elif kind == b'IDAT': chunks.extend(body)
        pos += length + 12
    raw = zlib.decompress(chunks)
    rows, prev, stride = [], bytearray(640 * 3), 640 * 3
    for y in range(480):
        method = raw[y*(stride+1)]
        row = bytearray(raw[y*(stride+1)+1:(y+1)*(stride+1)])
        for x in range(stride):
            a = row[x-3] if x >= 3 else 0
            b = prev[x]
            c = prev[x-3] if x >= 3 else 0
            if method == 1: row[x] = (row[x] + a) & 255
            elif method == 2: row[x] = (row[x] + b) & 255
            elif method == 3: row[x] = (row[x] + ((a+b)//2)) & 255
            elif method == 4:
                p = a+b-c
                distances = (abs(p-a), abs(p-b), abs(p-c))
                value = (a, b, c)[distances.index(min(distances))]
                row[x] = (row[x] + value) & 255
            else: assert method == 0
        rows.append(bytes(row)); prev = row
    return b''.join(rows)


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.rom = self.root / 'rom.bin'
        self.rom.write_bytes(rom_image())
        self.log = (self.root / 'guest.log').open('w+')
        self.agent = X16Agent(EMULATOR, self.rom, fsroot=self.root, stderr=self.log)

    def tearDown(self):
        self.agent.close()
        self.log.close()
        self.temp.cleanup()

    def code(self, code, address=0x6000):
        self.agent.write_memory(address, bytes.fromhex(code))
        self.agent.request('set_registers', pc=address)

    def test_handshake_and_paused_state(self):
        self.assertEqual(self.agent.hello['name'], 'X16 AgentBridge')
        before = self.agent.request('status')
        time.sleep(.02)
        self.assertEqual(before, self.agent.request('status'))
        self.assertEqual(before['pc'], 0xc000)
        self.assertEqual(before['cycles'], 0)

    def test_cpu_execution_flags_and_stp(self):
        self.code('18a9ff6901db')  # CLC; LDA #$ff; ADC #1; STP
        result = self.agent.advance(20, unit='instructions')
        self.assertEqual(result['reason'], 'stp')
        self.assertEqual(result['instructions'], 4)
        self.assertEqual(result['state']['a'], 0)
        self.assertEqual(result['state']['p'] & 3, 3)
        self.assertTrue(result['state']['halted'])
        self.assertEqual(self.agent.advance()['cycles'], 0)

    def test_exact_instructions_and_cycle_rounding(self):
        self.code('eaeaeaea4c0060')
        result = self.agent.advance(3, unit='instructions')
        self.assertEqual((result['instructions'], result['cycles'], result['state']['pc']), (3, 6, 0x6003))
        result = self.agent.advance(1, unit='cycles')
        self.assertEqual(result['cycles'], 2)

    def test_execution_budget_and_wait(self):
        self.code('cb')  # WAI
        result = self.agent.advance(10, unit='instructions', max_cycles=100)
        self.assertEqual(result['reason'], 'cycle_limit')
        self.assertEqual(result['instructions'], 1)
        self.assertTrue(result['state']['waiting'])
        self.assertLessEqual(result['cycles'], 110)
        self.agent.request('reset')
        self.assertFalse(self.agent.request('status')['waiting'])

    def test_breakpoint_before_execution(self):
        self.code('eaeaeaea')
        self.agent.request('breakpoint', address=0x6002)
        r = self.agent.advance(50, unit='instructions')
        self.assertEqual((r['reason'], r['instructions'], r['state']['pc']), ('breakpoint', 2, 0x6002))
        self.assertEqual(self.agent.advance()['cycles'], 0)
        self.agent.request('breakpoint', action='remove', address=0x6002)
        self.assertEqual(self.agent.advance(1, unit='instructions')['state']['pc'], 0x6003)

    def test_breakpoint_bank_and_capacity(self):
        self.code('eaea')
        self.agent.request('breakpoint', address=0x6000, bank=2)
        self.assertEqual(self.agent.advance(1, unit='instructions')['reason'], 'target')
        for i in range(31): self.agent.request('breakpoint', address=0x6100+i)
        with self.assertRaises(AgentError): self.agent.request('breakpoint', address=0x6200)
        self.assertEqual(len(self.agent.request('breakpoint', action='list')['breakpoints']), 32)
        self.agent.request('breakpoint', action='clear')

    def test_memory_spaces_and_banks(self):
        for bank in (0, 10):
            self.agent.write_memory(8190, bytes([bank, 255]), space='banked_ram', bank=bank)
        self.assertEqual(self.agent.read_memory(8190, 2, space='banked_ram', bank=0), b'\x00\xff')
        self.assertEqual(self.agent.read_memory(8190, 2, space='banked_ram', bank=10), b'\x0a\xff')
        self.agent.request('set_registers', ram_bank=10, rom_bank=3)
        self.assertEqual(self.agent.read_memory(0, 2), b'\x0a\x03')
        self.assertEqual(self.agent.read_memory(0xbffe, 2), b'\x0a\xff')
        self.agent.write_memory(131071, b'\x42', space='vram')
        self.assertEqual(self.agent.read_memory(131071, space='vram'), b'B')
        self.assertEqual(self.agent.read_memory(0, 3, space='rom'), b'\x4c\x00\xc0')

    def test_read_has_no_io_side_effects(self):
        self.agent.write_memory(0x9f20, b'\x00\x00\x10')  # VRAM address=0, increment=1
        self.agent.write_memory(0, b'\x11\x22', space='vram')
        before = self.agent.read_memory(0x9f20, 3)
        self.agent.read_memory(0x9f23, 10)
        self.assertEqual(before, self.agent.read_memory(0x9f20, 3))

    def test_validation_is_atomic(self):
        self.agent.write_memory(0x6000, b'\x33\x44')
        for fields in ({'address': 0x6000, 'data': '12zz'},
                       {'address': 0x9eff, 'data': '0102', 'space': 'ram'},
                       {'address': 0, 'data': '12', 'space': 'rom'},
                       {'address': 0, 'data': '12', 'space': 'banked_ram', 'bank': 64}):
            with self.assertRaises(AgentError): self.agent.request('memory_write', **fields)
        self.assertEqual(self.agent.read_memory(0x6000, 2), b'\x33\x44')
        before = self.agent.request('status')
        with self.assertRaises(AgentError): self.agent.request('set_registers', pc=0x1234, a=256)
        self.assertEqual(before, self.agent.request('status'))

    def test_unknown_fields_and_types(self):
        bad = [('advance', {'count': 0}), ('advance', {'count': True}),
               ('advance', {'unit': 'seconds'}), ('advance', {'max_cycles': 100000001}),
               ('set_registers', {'typo': 3}), ('key', {'key': 'A', 'down': 1}),
               ('key', {'key': 'no such key', 'down': True}), ('mouse', {'dx': 256}),
               ('hello', {'oops': 1}), ('status', {'idontknow': 1}),
               ('memory_read', {'address': 0, 'length': 16385}), ('does_not_exist', {})]
        for command, fields in bad:
            with self.subTest(command=command, fields=fields), self.assertRaises(AgentError):
                self.agent.request(command, **fields)
        self.assertEqual(self.agent.request('status')['cycles'], 0)

    def test_reset_preserves_ram_and_resets_bank_readback(self):
        self.agent.write_memory(0x6000, b'\x5a')
        self.agent.request('set_registers', ram_bank=10, rom_bank=3)
        self.agent.request('reset')
        self.assertEqual(self.agent.read_memory(0, 2), b'\x00\x00')
        self.assertEqual(self.agent.read_memory(0x6000), b'Z')

    def test_key_queue_backpressure_and_reset(self):
        for i in range(15): self.agent.request('key', key='A', down=not (i % 2))
        with self.assertRaises(AgentError): self.agent.request('key', key='A', down=False)
        self.agent.request('reset')
        self.agent.request('key', key='A', down=False)

    def test_mouse_queue_backpressure_and_reset(self):
        for buttons in (1, 0, 2): self.agent.request('mouse', dx=5, dy=-6, buttons=buttons)
        with self.assertRaises(AgentError): self.agent.request('mouse', buttons=0)
        self.agent.request('reset')
        self.agent.request('mouse', buttons=0)

    def test_prg_loading_and_errors(self):
        path = self.root / ('test spaced.prg' if os.name == 'nt' else 'test "quoted".prg')
        path.write_bytes(bytes.fromhex('0060a942db'))
        r = self.agent.request('load_prg', path=str(path), set_pc=True)
        self.assertEqual(r, dict(address=0x6000, length=3))
        self.assertEqual(self.agent.advance(10, unit='instructions')['state']['a'], 0x42)
        before = self.agent.read_memory(0x6000, 3)
        path.write_bytes(bytes.fromhex('ff9e0102'))
        with self.assertRaises(AgentError): self.agent.request('load_prg', path=str(path))
        self.assertEqual(self.agent.read_memory(0x6000, 3), before)
        with self.assertRaises(AgentError): self.agent.request('load_prg', path=str(self.root/'missing'))

    def test_vera_timing_irq_and_frames(self):
        self.code('4c0060')
        self.agent.request('set_registers', p=0)
        self.agent.write_memory(0x9f26, b'\x01')  # enable VSYNC IRQ
        result = self.agent.advance(3)
        self.assertEqual((result['reason'], result['frames']), ('target', 3))
        self.assertGreater(self.agent.read_memory(0x400)[0], 0)
        self.assertTrue(self.agent.read_memory(0x9f27)[0] & 1)

    def test_sprite_collision_irq_without_capture(self):
        self.agent.write_memory(0x1000, b'\x01'*64, space='vram')
        # Two 8x8 sprites at (20,20), collision mask 1, z-depth 1, 8bpp.
        sprite = bytes.fromhex('8080140014001400')
        self.agent.write_memory(0x1fc00, sprite*2, space='vram')
        self.agent.write_memory(0x9f29, b'\x41')
        self.agent.advance(2)
        self.assertEqual(self.agent.read_memory(0x9f27)[0] & 0x14, 0x14)

    def test_ntsc_capture_completes_both_fields(self):
        self.agent.write_memory(0x9f29, b'\x02')
        self.agent.write_memory(0x9f2c, b'\x01')
        self.agent.write_memory(0x1fa02, bytes.fromhex('000f'), space='vram')
        self.agent.write_memory(0x9f25, b'\x02')
        self.agent.write_memory(0x9f2c, b'\x00')
        self.agent.write_memory(0x9f25, b'\x00')
        self.agent.advance(1)
        result = self.agent.screenshot(self.root/'ntsc.png')
        self.assertEqual(result['frames'], 3)
        pixels = png_pixels(self.root/'ntsc.png')
        # Safe-area pixels in both field parities must be fresh red.
        for y in (100, 101, 300, 301):
            offset = (y*640+300)*3
            self.assertEqual(pixels[offset:offset+3], b'\xff\x00\x00')

    def test_pcm_fifo_advances_without_audio_device(self):
        self.agent.write_memory(0x9f3b, b'\x0f')  # mono PCM, full volume
        self.agent.write_memory(0x9f3c, b'\x80')  # consume one byte per sample
        self.agent.write_memory(0x9f3d, b'\x55')
        self.assertFalse(self.agent.read_memory(0x9f3b)[0] & 0x40)
        self.agent.advance(1)
        self.assertTrue(self.agent.read_memory(0x9f3b)[0] & 0x40)

    def test_ym_timer_advances_without_audio_device(self):
        # Timer A, shortest period. Allow busy time between register writes.
        for register, value in ((0x10, 255), (0x11, 3), (0x14, 5)):
            self.agent.write_memory(0x9f40, bytes([register, value]))
            self.agent.advance(1000, unit='cycles')
        self.agent.advance(1)
        self.assertTrue(self.agent.read_memory(0x9f41)[0] & 1)
        self.agent.request('reset')
        self.assertFalse(self.agent.read_memory(0x9f41)[0] & 1)

    def test_screenshot_png_pixels_and_freshness(self):
        # Red border. Screenshot must advance two frames, with no window and even with warp enabled.
        self.agent.write_memory(0x1fa02, bytes.fromhex('000f'), space='vram')
        self.agent.write_memory(0x9f2c, b'\x01')
        self.agent.write_memory(0x9f29, b'\x01')  # enable VGA output
        self.agent.write_memory(0x9f25, b'\x02')  # DCSEL=1
        self.agent.write_memory(0x9f2c, b'\x00')  # VSTOP=0: entire frame is border
        self.agent.write_memory(0x9f25, b'\x00')
        path = self.root / 'screen.png'
        result = self.agent.screenshot(path)
        self.assertEqual((result['frames'], result['captured']), (2, True))
        pixels = png_pixels(path)
        self.assertEqual(pixels[:3], b'\xff\x00\x00')
        self.assertEqual(pixels, b'\xff\x00\x00' * (640*480))
        self.agent.write_memory(0x1fa02, bytes.fromhex('f000'), space='vram')
        self.agent.screenshot(path)
        self.assertEqual(png_pixels(path)[:3], b'\x00\xff\x00')

    def test_screenshot_respects_limits_and_breakpoints(self):
        path = self.root/'screen.png'
        self.agent.request('breakpoint', address=0xc000)
        r = self.agent.screenshot(path)
        self.assertEqual(r['reason'], 'breakpoint')
        self.assertFalse(r['captured']); self.assertFalse(path.exists())
        self.agent.request('breakpoint', action='clear')
        r = self.agent.screenshot(path, max_cycles=10)
        self.assertEqual(r['reason'], 'cycle_limit')
        self.assertFalse(path.exists())
        with self.assertRaises(AgentError): self.agent.screenshot(self.root/'missing'/'screen.png')

    def test_guest_output_does_not_corrupt_json(self):
        self.code('a9418dbb9fa97f8db99fdb')
        self.assertEqual(self.agent.advance(20, unit='instructions')['reason'], 'stp')
        self.log.flush(); self.log.seek(0)
        self.assertIn('AUser debug 1: $7f', self.log.read())
        self.assertEqual(self.agent.request('status')['a'], 127)

    def test_exit_retains_api(self):
        self.code('4cffff')
        r = self.agent.advance(5)
        self.assertEqual(r['reason'], 'exit')
        self.assertTrue(r['state']['halted'])
        self.agent.request('reset')
        self.assertFalse(self.agent.request('status')['halted'])
        self.assertFalse((self.root/'dump.bin').exists())

    def test_raw_malformed_recovery_and_limits(self):
        # The client's reader is the sole stdout owner even for raw requests.
        for raw in ('garbage', '{"cmd":"status","cmd":"quit"}',
                    '{"cmd":"status","id":"bad"}', '{"cmd":"status","id":01}',
                    '{"cmd":"status"}\x00', 'x'*65537):
            self.agent.process.stdin.write(raw+'\n'); self.agent.process.stdin.flush()
            response = json.loads(self.agent._responses.get(timeout=5))
            self.assertFalse(response['ok'])
        self.assertEqual(self.agent.request('status')['cycles'], 0)

    def test_65c816_native_registers(self):
        with X16Agent(EMULATOR, self.rom, options=['-c816'], stderr=self.log) as agent:
            s = agent.request('set_registers', e=0, p=0, a=0x1234, x=0x3456, y=0x5678, sp=0x8765, dp=0x123)
            self.assertEqual((s['a'], s['x'], s['sp'], s['dp']), (0x1234, 0x3456, 0x8765, 0x123))
            s = agent.request('set_registers', e=1)
            self.assertEqual((s['x'], s['sp']), (0x56, 0x165))

    def test_gen2_physical_memory_banks(self):
        with X16Agent(EMULATOR, self.rom, options=['-gs'], stderr=self.log) as agent:
            agent.write_memory(0x6000, b'\xa9\x42\xdb', space='ram', bank=2)
            agent.write_memory(0x6000, b'\x11', space='ram', bank=3)
            self.assertEqual(agent.read_memory(0x6000, bank=2), b'\xa9')
            self.assertEqual(agent.read_memory(0x6000, bank=3), b'\x11')
            agent.request('set_registers', pc=0x6000, k=2)
            result = agent.advance(10, unit='instructions')
            self.assertEqual((result['reason'], result['state']['a']), ('stp', 0x42))

    def test_startup_conflicts_and_missing_rom(self):
        with self.assertRaises(AgentError):
            X16Agent(EMULATOR, self.root/'missing.bin', stderr=self.log)
        with self.assertRaises(AgentError):
            X16Agent(EMULATOR, self.rom, options=['-testbench'], stderr=self.log)

    def test_normal_frontend_still_runs(self):
        image = rom_image()
        image[:8] = bytes.fromhex('a94e8dbb9f4cffff')
        self.rom.write_bytes(image)
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', SDL_RENDER_DRIVER='software')
        result = subprocess.run([EMULATOR, '-rom', str(self.rom), '-sound', 'none'],
                                cwd=self.root, env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('N', result.stdout)

    def test_warp_does_not_skip_agent_frames(self):
        with X16Agent(EMULATOR, self.rom, options=['-warp'], stderr=self.log) as agent:
            path = self.root/'warp.png'
            self.assertTrue(agent.screenshot(path)['captured'])
            self.assertEqual(png_pixels(path), b'\x00\x00\xff' * (640*480))

    def test_eof_terminates_cleanly(self):
        self.agent.process.stdin.close()
        self.agent.process.wait(timeout=5)
        self.assertEqual(self.agent.process.returncode, 0)

    def test_deterministic_initial_memory(self):
        with X16Agent(EMULATOR, self.rom, stderr=self.log) as other:
            for space in ('ram', 'banked_ram', 'vram'):
                self.assertEqual(self.agent.read_memory(100, 100, space=space),
                                 other.read_memory(100, 100, space=space))

    def test_client_timeout_terminates_process(self):
        self.agent.timeout = .001
        with self.assertRaises(TimeoutError):
            self.agent.advance(100000000, unit='cycles')
        self.assertIsNotNone(self.agent.process.poll())

    def test_client_rejects_reserved_and_oversized_fields(self):
        with self.assertRaises(ValueError): self.agent.request('hello', id=99)
        with self.assertRaises(ValueError): self.agent.request('paste', text='x'*65536)
        self.assertEqual(self.agent.request('status')['cycles'], 0)

    @unittest.skipIf(os.name == 'nt', 'Legacy wrapper uses POSIX SIGALRM')
    def test_legacy_testbench_regressions(self):
        import importlib.util
        import signal
        import io
        testbench_dir = Path(__file__).resolve().parents[1]/'testbench'
        sys.path.insert(0, str(testbench_dir))
        import testbench
        instances = []
        image = rom_image()
        image[0x3ffc:0x3ffe] = struct.pack('<H', 0xfffd)
        self.rom.write_bytes(image)
        def launch(path, options=()):
            e = testbench.X16TestBench(EMULATOR, [*options, '-rom', str(self.rom), '-zeroram'])
            instances.append(e)
            return e
        try:
            suite = unittest.TestSuite()
            for name in ('selftest', 'example'):
                spec = importlib.util.spec_from_file_location(name, testbench_dir/(name+'.py'))
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                module.X16TestBench = launch
                suite.addTests(unittest.defaultTestLoader.loadTestsFromModule(module))
            stream = io.StringIO()
            result = unittest.TextTestRunner(stream=stream).run(suite)
            self.assertTrue(result.wasSuccessful(), stream.getvalue())
        finally:
            signal.alarm(0)
            for e in instances:
                e.emu.terminate(); e.emu.wait(timeout=5)


if __name__ == '__main__':
    # An invalid backend must not matter: agent mode opens neither video nor audio.
    os.environ['SDL_VIDEODRIVER'] = 'agent-test-invalid'
    os.environ['SDL_AUDIODRIVER'] = 'agent-test-invalid'
    unittest.main(verbosity=2)
