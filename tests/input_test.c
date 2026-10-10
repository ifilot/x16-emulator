// Test real keyboard translation and mouse packets independently of an X16 ROM.
#include <stdbool.h>
#include <stdio.h>
#include <SDL.h>
#include "i2c.h"
#include "keyboard.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
bool smc_requested_reset, log_keyboard;
void smc_i2c_data(uint8_t v) { (void)v; }
void rtc_i2c_data(uint8_t v) { (void)v; }
uint8_t smc_read(void) { return 0; }
uint8_t rtc_read(void) { return 0; }
void smc_write(void) { }
void rtc_write(void) { }
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    i2c_reset_state();
    handle_keyboard(true, SDLK_a, SDL_SCANCODE_A);
    handle_keyboard(false, SDLK_a, SDL_SCANCODE_A);
    CHECK(i2c_kbd_buffer_count() == 2);
    CHECK(i2c_kbd_buffer_next() == 31);
    CHECK(i2c_kbd_buffer_next() == (31 | 128));
    CHECK(i2c_kbd_buffer_next() == 0);
    handle_keyboard(true, SDLK_RETURN, SDL_SCANCODE_RETURN);
    CHECK(i2c_kbd_buffer_next() == 43);
    handle_keyboard(true, 0, SDL_SCANCODE_UNKNOWN);
    CHECK(i2c_kbd_buffer_count() == 0);
    for (int i = 1; i <= 16; ++i) i2c_kbd_buffer_add(i);
    CHECK(i2c_kbd_buffer_count() == 15);
    for (int i = 1; i <= 15; ++i) CHECK(i2c_kbd_buffer_next() == i);
    mouse_move(12, -20); mouse_button_down(0); mouse_send_state();
    CHECK(i2c_mse_buffer_count() == 4);
    CHECK(i2c_mse_buffer_next() == 9);
    CHECK(i2c_mse_buffer_next() == 12);
    CHECK(i2c_mse_buffer_next() == 20);
    CHECK(i2c_mse_buffer_next() == 0);
    mouse_button_up(0); mouse_send_state();
    CHECK(i2c_mse_buffer_next() == 8);
    CHECK(i2c_mse_buffer_next() == 0);
    CHECK(i2c_mse_buffer_next() == 0);
    CHECK(i2c_mse_buffer_next() == 0);
    // Horizontal-only movement must split across multiple packets.
    mouse_move(600, 0); mouse_send_state();
    CHECK(i2c_mse_buffer_count() == 12);
    for (int i = 0; i < 3; ++i) {
        CHECK(i2c_mse_buffer_next() == 8);
        CHECK(i2c_mse_buffer_next() == (i == 2 ? 90 : 255));
        CHECK(i2c_mse_buffer_next() == 0); CHECK(i2c_mse_buffer_next() == 0);
    }
    // Full queue preserves unsent movement rather than silently dropping it.
    for (int i = 0; i < 12; ++i) i2c_mse_buffer_add(0);
    mouse_move(30, 0); mouse_send_state();
    CHECK(i2c_mse_buffer_count() == 12);
    i2c_mse_buffer_flush(); mouse_send_state();
    CHECK(i2c_mse_buffer_next() == 8); CHECK(i2c_mse_buffer_next() == 30);
    i2c_reset_state(); mouse_send_state();
    CHECK(i2c_mse_buffer_next() == 8);
    CHECK(i2c_mse_buffer_next() == 0); CHECK(i2c_mse_buffer_next() == 0);
    CHECK(i2c_mse_buffer_next() == 0);
    puts("input tests passed");
    return 0;
}
