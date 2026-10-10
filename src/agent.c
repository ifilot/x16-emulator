// X16 AgentBridge. One flat JSON request and response per line; paused between requests.
#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "agent_json.h"
#include "glue.h"
#include "memory.h"
#include "video.h"
#include "audio.h"
#include "keyboard.h"
#include "i2c.h"
#include "cpu/fake6502.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#define fdopen _fdopen
#else
#include <unistd.h>
#endif

#define LINE_MAX_BYTES 65536
#define MEMORY_MAX_BYTES 16384
#define RUN_MAX_CYCLES 100000000
#define BREAKPOINT_MAX 32
bool agent_mode;
static FILE *output;
static char line[LINE_MAX_BYTES + 2], capture_path[LINE_MAX_BYTES];
static uint64_t cycles, instructions, frames;
static uint64_t start_cycles, start_instructions, start_frames, target, budget;
static uint32_t last_clock;
static int64_t request_id;
static bool has_id, running, quitting, halted, capturing;
static enum { RUN_CYCLES, RUN_INSTRUCTIONS, RUN_FRAMES } unit;
static const char *stop_reason = "reset";
typedef struct { uint16_t address; int bank; } breakpoint;
static breakpoint breakpoints[BREAKPOINT_MAX];
static size_t breakpoint_count;
extern bool pasting_bas;
static char paste_buffer[LINE_MAX_BYTES];

bool agent_render_video(void) { return capturing; }

bool agent_open(void) {
#ifdef __EMSCRIPTEN__
    fprintf(stderr, "-agent is supported only by native builds.\n");
    return false;
#else
    fflush(stdout);
    int fd = dup(fileno(stdout));
    if (fd < 0) return false;
    output = fdopen(fd, "w");
    if (!output) { close(fd); return false; }
    if (dup2(fileno(stderr), fileno(stdout)) < 0) { fclose(output); output = NULL; return false; }
    setvbuf(output, NULL, _IOLBF, 0);
    return true;
#endif
}
void agent_close(void) {
    if (output) { fclose(output); output = NULL; }
}
static void prefix(bool ok) {
    fputs("{\"id\":", output);
    if (has_id) fprintf(output, "%" PRId64, request_id);
    else fputs("null", output);
    fprintf(output, ",\"ok\":%s,", ok ? "true" : "false");
}
static void error(const char *message) {
    prefix(false);
    // All error strings are controlled literals.
    fprintf(output, "\"error\":\"%s\"}\n", message);
    fflush(output);
}
static void empty(void) {
    prefix(true); fputs("\"result\":{}}\n", output); fflush(output);
}
static void state(void) {
    fprintf(output, "{\"pc\":%u,\"a\":%u,\"x\":%u,\"y\":%u,\"sp\":%u,"
        "\"p\":%u,\"dp\":%u,\"db\":%u,\"k\":%u,\"e\":%u,\"c816\":%s,"
        "\"ram_bank\":%u,\"rom_bank\":%u,\"waiting\":%s,\"halted\":%s,"
        "\"cycles\":%" PRIu64 ",\"instructions\":%" PRIu64 ",\"frames\":%" PRIu64 ",\"mhz\":%u}",
        regs.pc, regs.c, regs.x, regs.y, regs.sp, regs.status, regs.dp, regs.db, regs.k, regs.e,
        regs.is65c816 ? "true" : "false", memory_get_ram_bank(), memory_get_rom_bank(),
        waiting ? "true" : "false", halted ? "true" : "false", cycles, instructions, frames, MHZ);
}
static void state_response(void) {
    prefix(true); fputs("\"result\":", output); state(); fputs("}\n", output); fflush(output);
}
static bool integer(const aj_object *o, const char *key, int64_t min, int64_t max, int64_t fallback, int64_t *value) {
    const aj_field *f = aj_get(o, key);
    if (!f) { *value = fallback; return true; }
    if (f->type != AJ_INTEGER || f->integer < min || f->integer > max) return false;
    *value = f->integer; return true;
}
static const char *str(const aj_object *o, const char *key, const char *fallback) {
    const aj_field *f = aj_get(o, key);
    return f ? (f->type == AJ_STRING ? f->string : NULL) : fallback;
}
static bool boolean(const aj_object *o, const char *key, bool fallback, bool *value) {
    const aj_field *f = aj_get(o, key);
    if (!f) { *value = fallback; return true; }
    if (f->type != AJ_BOOL) return false;
    *value = f->integer != 0; return true;
}
static bool at_breakpoint(void) {
    int bank = regs.k ? regs.k : (regs.pc >= 0xc000 ? memory_get_rom_bank() :
        (regs.pc >= 0xa000 ? memory_get_ram_bank() : 0));
    for (size_t i = 0; i < breakpoint_count; ++i)
        if (breakpoints[i].address == regs.pc && (breakpoints[i].bank < 0 || breakpoints[i].bank == bank)) return true;
    return false;
}
void agent_stop(const char *reason) {
    stop_reason = reason;
    halted = !strcmp(reason, "stp") || !strcmp(reason, "exit") || !strcmp(reason, "poweroff");
}
static void finish(const char *reason) {
    // Resolve pending chip time before any observer reads PCM/YM state.
    audio_render();
    running = false;
    stop_reason = reason;
    bool captured = false;
    if (capturing && !strcmp(reason, "target")) {
        captured = video_capture_png(capture_path);
        if (!captured) { capturing = false; error("Cannot write screenshot"); return; }
    }
    prefix(true);
    fprintf(output, "\"result\":{\"reason\":\"%s\",\"cycles\":%" PRIu64
        ",\"instructions\":%" PRIu64 ",\"frames\":%" PRIu64 ",\"captured\":%s,\"state\":",
        reason, cycles - start_cycles, instructions - start_instructions, frames - start_frames,
        captured ? "true" : "false");
    state(); fputs("}}\n", output); fflush(output);
    capturing = false;
}
void agent_after_step(bool frame, bool instruction) {
    cycles += (uint32_t)(clockticks6502 - last_clock);
    last_clock = clockticks6502;
    instructions += instruction;
    frames += frame;
    if (!running) return;
    if (halted) { finish(stop_reason); return; }
    if (at_breakpoint()) { finish("breakpoint"); return; }
    uint64_t elapsed = unit == RUN_CYCLES ? cycles - start_cycles :
        (unit == RUN_INSTRUCTIONS ? instructions - start_instructions : frames - start_frames);
    if (elapsed >= target) finish("target");
    else if (cycles - start_cycles >= budget) finish("cycle_limit");
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void memory_command(const aj_object *o, bool write) {
    if (!aj_allowed(o, write ? "id cmd space bank address data" : "id cmd space bank address length")) {
        error("Unknown memory field"); return;
    }
    const char *space = str(o, "space", "cpu");
    const char *data = write ? str(o, "data", NULL) : NULL;
    int64_t address, length, bank;
    if (!space || !integer(o, "address", 0, 131071, -1, &address) || address < 0 ||
        !integer(o, "bank", 0, 255, 0, &bank)) { error("Invalid memory address or bank"); return; }
    if (write) {
        if (!data || !*data || strlen(data) % 2 || strlen(data) > MEMORY_MAX_BYTES * 2) {
            error("data must be a nonempty hex string of at most 16384 bytes"); return;
        }
        length = strlen(data) / 2;
        for (int64_t i = 0; i < length * 2; ++i)
            if (hex(data[i]) < 0) { error("Invalid hex data"); return; }
    } else if (!integer(o, "length", 1, MEMORY_MAX_BYTES, 1, &length)) {
        error("Invalid memory length"); return;
    }
    enum { CPU, RAM_SPACE, BRAM_SPACE, ROM_SPACE, VRAM_SPACE } kind;
    uint32_t size;
    if (!strcmp(space, "cpu")) {
        kind = CPU; size = 65536;
        if (bank >= num_banks || (bank && !is_gen2)) { error("CPU bank is not installed"); return; }
    } else if (!strcmp(space, "ram")) {
        kind = RAM_SPACE; size = bank ? 65536 : 0x9f00;
        if (bank >= num_banks) { error("RAM bank is not installed"); return; }
    } else if (!strcmp(space, "banked_ram")) {
        kind = BRAM_SPACE; size = 8192;
        if (bank >= num_ram_banks) { error("Banked RAM bank is not installed"); return; }
    } else if (!strcmp(space, "rom")) {
        kind = ROM_SPACE; size = 16384;
        if (bank >= NUM_ROM_BANKS || write) { error("ROM is read-only; banks 0-31 supported"); return; }
    } else if (!strcmp(space, "vram")) {
        kind = VRAM_SPACE; size = 131072;
        if (bank) { error("VRAM has no banks"); return; }
    } else { error("Unknown memory space"); return; }
    if ((uint64_t)address + length > size || (write && kind == CPU && !bank && address + length > 0xc000)) {
        error("Memory range exceeds writable space"); return;
    }
    if (!write) { prefix(true); fputs("\"result\":{\"data\":\"", output); }
    for (int64_t i = 0; i < length; ++i) {
        uint32_t a = (uint32_t)(address + i);
        uint8_t v = 0;
        if (write) v = (hex(data[i * 2]) << 4) | hex(data[i * 2 + 1]);
        switch (kind) {
            case CPU:
                if (write) write6502(a, bank, v);
                else v = debug_read6502(a, bank, USE_CURRENT_X16_BANK);
                break;
            case RAM_SPACE:
                if (write) RAM[bank * BANK_SIZE + a] = v;
                else v = RAM[bank * BANK_SIZE + a];
                break;
            case BRAM_SPACE:
                if (write) BRAM[bank * 8192 + a] = v;
                else v = BRAM[bank * 8192 + a];
                break;
            case ROM_SPACE: v = ROM[bank * 16384 + a]; break;
            case VRAM_SPACE:
                if (write) video_space_write(a, v);
                else v = video_space_read(a);
                break;
        }
        if (!write) fprintf(output, "%02x", v);
    }
    if (write) empty();
    else { fputs("\"}}\n", output); fflush(output); }
}
static void set_registers(const aj_object *o) {
    if (!aj_allowed(o, "id cmd pc a x y sp p dp db k e ram_bank rom_bank")) { error("Unknown register"); return; }
    struct regs next = regs;
    int64_t rb, romb, v;
    if (!integer(o, "ram_bank", 0, num_ram_banks - 1, memory_get_ram_bank(), &rb) ||
        !integer(o, "rom_bank", 0, 255, memory_get_rom_bank(), &romb)) { error("Invalid bank"); return; }
#define SET(key, member, max) do { \
    if (!integer(o, key, 0, max, next.member, &v)) { error("Invalid register value"); return; } \
    next.member = v; \
} while (0)
    SET("pc", pc, 65535); SET("a", c, regs.is65c816 ? 65535 : 255);
    SET("x", x, regs.is65c816 ? 65535 : 255); SET("y", y, regs.is65c816 ? 65535 : 255);
    SET("sp", sp, 65535); SET("p", status, 255); SET("dp", dp, regs.is65c816 ? 65535 : 0);
    SET("db", db, regs.is65c816 ? 255 : 0); SET("k", k, is_gen2 ? num_banks - 1 : 0);
    SET("e", e, 1);
#undef SET
    if (!next.is65c816 && !next.e) { error("65C02 requires emulation mode"); return; }
    if (next.e) {
        next.sp = 0x100 | (next.sp & 255);
        next.status |= FLAG_INDEX_WIDTH | FLAG_MEMORY_WIDTH;
    }
    if (next.status & FLAG_INDEX_WIDTH) { next.x &= 255; next.y &= 255; }
    regs = next;
    // Keep architectural bank registers and their RAM readback consistent.
    write6502(0, 0, rb); write6502(1, 0, romb);
    if (aj_get(o, "pc")) { halted = false; waiting = 0; }
    state_response();
}
static void load_prg(const aj_object *o) {
    const char *path = str(o, "path", NULL);
    int64_t address;
    bool set_pc;
    if (!aj_allowed(o, "id cmd path address set_pc") || !path || !*path ||
        !integer(o, "address", 2, 0x9eff, -1, &address) || !boolean(o, "set_pc", false, &set_pc)) {
        error("Invalid PRG arguments"); return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { error("Cannot open PRG"); return; }
    uint8_t bytes[0x9f01];
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    bool failed = ferror(f);
    fclose(f);
    if (failed || n < 3) { error("Invalid or empty PRG"); return; }
    if (address < 0) address = bytes[0] | (bytes[1] << 8);
    if (address < 2 || (uint64_t)address + n - 2 > 0x9f00) { error("PRG does not fit in low RAM"); return; }
    for (size_t i = 2; i < n; ++i) write6502(address + i - 2, 0, bytes[i]);
    if (set_pc) { regs.pc = address; regs.k = 0; waiting = 0; halted = false; }
    prefix(true); fprintf(output, "\"result\":{\"address\":%" PRId64 ",\"length\":%zu}}\n", address, n - 2);
    fflush(output);
}
static void dispatch(const aj_object *o) {
    const char *cmd = str(o, "cmd", NULL);
    if (!cmd || !*cmd) { error("cmd must be a string"); return; }
    if (!strcmp(cmd, "hello")) {
        if (!aj_allowed(o, "id cmd")) { error("Unknown hello field"); return; }
        prefix(true);
        fputs("\"result\":{\"name\":\"X16 AgentBridge\",\"protocol\":1,\"paused\":true,"
            "\"max_request_bytes\":65536,\"max_memory_bytes\":16384,\"max_cycles\":100000000,"
            "\"commands\":[\"hello\",\"status\",\"set_registers\",\"memory_read\",\"memory_write\","
            "\"load_prg\",\"reset\",\"advance\",\"breakpoint\",\"key\",\"mouse\",\"paste\",\"screenshot\",\"quit\"]}}\n", output);
    } else if (!strcmp(cmd, "status")) {
        if (!aj_allowed(o, "id cmd")) { error("Unknown status field"); return; }
        state_response();
    } else if (!strcmp(cmd, "memory_read") || !strcmp(cmd, "memory_write")) {
        memory_command(o, !strcmp(cmd, "memory_write"));
    } else if (!strcmp(cmd, "set_registers")) set_registers(o);
    else if (!strcmp(cmd, "load_prg")) load_prg(o);
    else if (!strcmp(cmd, "reset")) {
        if (!aj_allowed(o, "id cmd")) { error("Unknown reset field"); return; }
        machine_reset();
        halted = false; pasting_bas = false;
        stop_reason = "reset"; state_response();
    } else if (!strcmp(cmd, "advance") || !strcmp(cmd, "screenshot")) {
        bool screenshot = !strcmp(cmd, "screenshot");
        if (!aj_allowed(o, screenshot ? "id cmd path max_cycles" : "id cmd unit count max_cycles")) {
            error("Unknown execution field"); return;
        }
        const char *u = str(o, "unit", "frames");
        const char *path = screenshot ? str(o, "path", NULL) : NULL;
        int64_t count, max;
        if (!u || !integer(o, "count", 1, RUN_MAX_CYCLES, 1, &count) ||
            !integer(o, "max_cycles", 1, RUN_MAX_CYCLES, RUN_MAX_CYCLES, &max) ||
            (screenshot && (!path || !*path))) { error("Invalid execution arguments"); return; }
        if (!strcmp(u, "cycles")) unit = RUN_CYCLES;
        else if (!strcmp(u, "instructions")) unit = RUN_INSTRUCTIONS;
        else if (!strcmp(u, "frames")) unit = RUN_FRAMES;
        else { error("unit must be cycles, instructions, or frames"); return; }
        capturing = screenshot;
        if (screenshot) {
            strcpy(capture_path, path); unit = RUN_FRAMES;
            // One partial interval, then a full VGA frame or two NTSC fields.
            count = (video_get_dc_value(0) & 2) ? 3 : 2;
        }
        target = count; budget = max;
        start_cycles = cycles; start_instructions = instructions; start_frames = frames;
        running = true;
        if (halted) finish(stop_reason);
        else if (at_breakpoint()) finish("breakpoint");
    } else if (!strcmp(cmd, "breakpoint")) {
        const char *action = str(o, "action", "add");
        int64_t address, bank;
        if (!aj_allowed(o, "id cmd action address bank") || !action ||
            !integer(o, "address", 0, 65535, -1, &address) || !integer(o, "bank", -1, 255, -1, &bank)) {
            error("Invalid breakpoint"); return;
        }
        if (!strcmp(action, "clear")) breakpoint_count = 0;
        else if (!strcmp(action, "add") || !strcmp(action, "remove")) {
            if (address < 0) { error("Breakpoint address required"); return; }
            size_t i = 0;
            while (i < breakpoint_count && (breakpoints[i].address != address || breakpoints[i].bank != bank)) ++i;
            if (!strcmp(action, "add") && i == breakpoint_count) {
                if (breakpoint_count == BREAKPOINT_MAX) { error("Breakpoint table full"); return; }
                breakpoints[breakpoint_count++] = (breakpoint){address, bank};
            } else if (!strcmp(action, "remove") && i < breakpoint_count) {
                breakpoints[i] = breakpoints[--breakpoint_count];
            }
        } else if (strcmp(action, "list")) { error("Unknown breakpoint action"); return; }
        prefix(true); fputs("\"result\":{\"breakpoints\":[", output);
        for (size_t i = 0; i < breakpoint_count; ++i)
            fprintf(output, "%s{\"address\":%u,\"bank\":%d}", i ? "," : "", breakpoints[i].address, breakpoints[i].bank);
        fputs("]}}\n", output);
    } else if (!strcmp(cmd, "key")) {
        const char *key = str(o, "key", NULL);
        bool down;
        if (!aj_allowed(o, "id cmd key down") || !key || !aj_get(o, "down") || !boolean(o, "down", false, &down)) {
            error("key and boolean down required"); return;
        }
        SDL_Scancode scancode = SDL_GetScancodeFromName(key);
        if (!keynum_from_SDL_Scancode(scancode)) { error("Unsupported key name"); return; }
        if (i2c_kbd_buffer_count() >= 15) { error("Keyboard queue full; advance the machine"); return; }
        handle_keyboard(down, SDL_GetKeyFromScancode(scancode), scancode); empty();
    } else if (!strcmp(cmd, "mouse")) {
        int64_t dx, dy, buttons, wheel;
        if (!aj_allowed(o, "id cmd dx dy buttons wheel") ||
            !integer(o, "dx", -255, 255, 0, &dx) || !integer(o, "dy", -255, 255, 0, &dy) ||
            !integer(o, "buttons", 0, 7, 0, &buttons) || !integer(o, "wheel", -7, 7, 0, &wheel)) {
            error("Invalid mouse packet"); return;
        }
        int packet_size = mouse_get_device_id() ? 4 : 3;
        if (i2c_mse_buffer_count() >= 16 - packet_size) { error("Mouse queue full; advance the machine"); return; }
        for (int i = 0; i < 3; ++i) {
            if (buttons & (1 << i)) mouse_button_down(i); else mouse_button_up(i);
        }
        mouse_move(dx, dy); mouse_set_wheel(wheel); mouse_send_state(); empty();
    } else if (!strcmp(cmd, "paste")) {
        const char *text = str(o, "text", NULL);
        if (!aj_allowed(o, "id cmd text") || !text || !*text) { error("Nonempty text required"); return; }
        if (pasting_bas) { error("Paste already in progress"); return; }
        strcpy(paste_buffer, text); machine_paste(paste_buffer, false); empty();
    } else if (!strcmp(cmd, "quit")) {
        if (!aj_allowed(o, "id cmd")) { error("Unknown quit field"); return; }
        empty(); quitting = true;
    } else error("Unknown command");
    fflush(output);
}
bool agent_before_step(uint32_t *old_clock) {
    while (!running && !quitting) {
        // Drain overlong requests to the next newline; never execute a truncated command.
        size_t n = 0;
        bool too_long = false, nul = false;
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (!c) nul = true;
            if (n < LINE_MAX_BYTES) line[n++] = c; else too_long = true;
        }
        if (c == EOF && !n && !too_long) return false;
        line[n] = 0;
        has_id = false;
        aj_object o;
        if (too_long || nul || !aj_parse(line, &o)) { error("Invalid flat JSON request or request too large"); continue; }
        const aj_field *id = aj_get(&o, "id");
        if (id && id->type != AJ_INTEGER && id->type != AJ_NULL) { error("id must be an integer or null"); continue; }
        if (id && id->type == AJ_INTEGER) { request_id = id->integer; has_id = true; }
        dispatch(&o);
        *old_clock = last_clock = clockticks6502;
    }
    return !quitting;
}
