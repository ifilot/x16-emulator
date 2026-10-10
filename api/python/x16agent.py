"""Standard-library client for the X16 AgentBridge native JSON API."""
import json
import queue
import subprocess
import threading


class AgentError(RuntimeError):
    """The emulator rejected a request without changing its validated inputs."""


class X16Agent:
    def __init__(self, executable, rom, *, fsroot=None, options=(), timeout=15, stderr=None):
        if stderr == subprocess.PIPE:
            raise ValueError('Pass a writable stderr file or inherit stderr; an undrained PIPE can block guest output')
        command = [str(executable), '-agent', '-rom', str(rom)]
        if fsroot is not None:
            command += ['-fsroot', str(fsroot)]
        command += list(options)
        self.timeout = timeout
        self._next_id = 0
        self._lock = threading.Lock()
        self._responses = queue.Queue()
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=stderr, text=True, encoding='utf-8', bufsize=1)
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        try:
            self.hello = self.request('hello')
            if self.hello.get('protocol') != 1:
                raise AgentError('Unsupported AgentBridge protocol version')
        except BaseException:
            self.close()
            raise

    def _read(self):
        try:
            for line in self.process.stdout:
                self._responses.put(line)
        finally:
            self._responses.put(None)

    def request(self, command, **fields):
        """Execute one synchronous command; return its result or raise AgentError.

        A timeout terminates the process, so an outstanding execution response can
        never accidentally be interpreted as the response to a later command.
        """
        with self._lock:
            if 'id' in fields or 'cmd' in fields:
                raise ValueError('id and cmd are reserved')
            self._next_id += 1
            payload = json.dumps(dict(id=self._next_id, cmd=command, **fields), ensure_ascii=True, allow_nan=False)
            limit = getattr(self, 'hello', {}).get('max_request_bytes', 65536)
            if len(payload.encode('utf-8')) > limit:
                raise ValueError('Request exceeds protocol size limit')
            try:
                self.process.stdin.write(payload + '\n')
                self.process.stdin.flush()
                line = self._responses.get(timeout=self.timeout)
            except queue.Empty:
                self.process.kill()
                self.process.wait()
                raise TimeoutError('AgentBridge response timed out; emulator terminated') from None
            except (BrokenPipeError, OSError):
                raise AgentError('AgentBridge process disconnected') from None
            if line is None:
                raise AgentError(f'AgentBridge process disconnected (exit={self.process.poll()})')
            try:
                response = json.loads(line)
            except (ValueError, TypeError):
                self.process.kill()
                self.process.wait()
                raise AgentError('Invalid AgentBridge response; emulator terminated') from None
            if response.get('id') != self._next_id:
                self.process.kill()
                self.process.wait()
                raise AgentError('Mismatched response ID')
            if not response.get('ok'):
                raise AgentError(response.get('error', 'Unknown API error'))
            return response['result']

    def read_memory(self, address, length=1, *, space='cpu', bank=0):
        return bytes.fromhex(self.request('memory_read', address=address, length=length, space=space, bank=bank)['data'])

    def write_memory(self, address, data, *, space='cpu', bank=0):
        return self.request('memory_write', address=address, data=bytes(data).hex(), space=space, bank=bank)

    def advance(self, count=1, *, unit='frames', max_cycles=100000000):
        return self.request('advance', count=count, unit=unit, max_cycles=max_cycles)

    def screenshot(self, path, *, max_cycles=100000000):
        return self.request('screenshot', path=str(path), max_cycles=max_cycles)

    def close(self):
        if self.process.poll() is None:
            try:
                self.request('quit')
            except (AgentError, TimeoutError, OSError):
                self.process.kill()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self._reader.join(timeout=2)
        for pipe in (self.process.stdin, self.process.stdout):
            try:
                pipe.close()
            except OSError:
                # A process that rejects startup may exit before buffered stdin
                # is flushed; cleanup must preserve the original API error.
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
