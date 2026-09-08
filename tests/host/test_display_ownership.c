#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef REPO_ROOT
#error "REPO_ROOT must point to the repository root"
#endif

static char *read_source(const char *relative_path)
{
    char path[1024];
    int written = snprintf(path, sizeof(path), "%s/%s", REPO_ROOT, relative_path);
    assert(written > 0 && (size_t)written < sizeof(path));
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1U);
    assert(text != NULL);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    fclose(file);
    return text;
}

static void assert_not_present(const char *text, const char *needle)
{
    assert(strstr(text, needle) == NULL);
}

int main(void)
{
    char *idle = read_source("main/app/julia_idle_display.c");
    assert_not_present(idle, "julia_backlight_");
    assert_not_present(idle, "lvgl_port_set_display_off");
    assert_not_present(idle, "julia_avatar_set_dozing");
    free(idle);

    char *motion = read_source("main/context/julia_motion.c");
    assert_not_present(motion, "julia_idle_display_note_activity");
    assert_not_present(motion, "julia_backlight_");
    assert_not_present(motion, "lvgl_port_set_display_off");
    assert(strstr(motion, "julia_fsm_runtime_post(EVT_MOTION_WAKE)") != NULL);
    free(motion);

    char *night = read_source("main/context/julia_night_schedule.c");
    assert_not_present(night, "julia_idle_display_note_activity");
    free(night);

    char *voice = read_source("main/voice/voice_service.c");
    char *session_end = strstr(voice, "static void voice_service_on_session_end");
    assert(session_end != NULL);
    char *next_function = strstr(session_end, "static esp_err_t voice_service_enqueue");
    assert(next_function != NULL && next_function > session_end);
    char saved = *next_function;
    *next_function = '\0';
    assert_not_present(session_end, "julia_idle_display_note_activity");
    *next_function = saved;
    free(voice);

    char *runtime = read_source("main/fsm/julia_fsm_runtime.c");
    assert(strstr(runtime, "case FSM_PRESENT_S6_SLEEP:") != NULL);
    assert(strstr(runtime, "julia_backlight_set(0);") != NULL);
    assert(strstr(runtime, "lvgl_port_set_display_off(true)") != NULL);
    free(runtime);

    puts("PASS: S6 display hardware remains exclusively owned by FSM presentation");
    return 0;
}
