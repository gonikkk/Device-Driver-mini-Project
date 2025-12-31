#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>
#include "font_header.h"

#define DEV_OLED    "/dev/ssd1306_driver"
#define DEV_ROTARY  "/dev/rotary_device_driver"
#define DEV_RTC     "/dev/ds1302_driver"

#define SCREEN_W 128
#define SCREEN_H 64
static unsigned char fb[1024];

typedef enum { STATE_MENU, STATE_CLOCK, STATE_WORLD, STATE_GAME } AppState;
typedef enum { CLOCK_VIEW, CLOCK_EDIT } ClockMode;

/* ========== 전역 ========== */
static int fd_oled = -1, fd_rot = -1, fd_rtc = -1;

static AppState current_state = STATE_MENU;
static ClockMode clock_mode   = CLOCK_VIEW;

static long rotary_val = 0;
static long last_rotary_val = 0;
static long rotary_delta = 0;

static int  menu_index = 0;
static int  menu_acc = 0;
static const char *menu_items[] = {"CLOCK", "WORLD", "GAME"};

static struct timespec press_start;
static int is_holding = 0;

/* RTC 캐시 */
static char rtc_cache[32] = "2000-01-01 00:00:00";
static struct timespec last_rtc_poll = {0};

/* CLOCK 수정 */
static int edit_year, edit_mon, edit_day, edit_hour, edit_min, edit_sec;
static int edit_field = 0;

/* WORLD 도시 테이블 (국기 태그 + 이름 + SEOUL 기준 오프셋) */
typedef struct {
    const char *tag;        // 국기 느낌 태그
    const char *name;
    int offset_hours;       // SEOUL 기준
} City;

static City cities[] = {
    {"[KR]", "SEOUL",     0},
    {"[JP]", "TOKYO",     0},
    {"[CN]", "BEIJING",  -1},
    {"[VN]", "HANOI",    -2},
    {"[FR]", "PARIS",    -8},
    {"[US]", "NEW YORK", -14},
};
#define CITY_COUNT (int)(sizeof(cities)/sizeof(cities[0]))
static int world_city = 0;
static int world_acc  = 0;

/* GAME */
static int player_x = 60;
static int obs_x = 30, obs_y = -10;
static int score = 0;
static int game_over = 0;

/* ========== 유틸 ========== */
static long diff_ms(struct timespec a, struct timespec b) {
    long sec  = a.tv_sec - b.tv_sec;
    long nsec = a.tv_nsec - b.tv_nsec;
    return sec * 1000 + nsec / 1000000;
}

static void poll_rtc_if_due(int period_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    int first = (last_rtc_poll.tv_sec == 0 && last_rtc_poll.tv_nsec == 0);
    if (!first) {
        if (diff_ms(now, last_rtc_poll) < period_ms) return;
    }
    last_rtc_poll = now;

    char tmp[64] = {0};

    int n = pread(fd_rtc, tmp, sizeof(tmp) - 1, 0);
    if (n <= 0) return;

    /* ds1302_driver는 보통 "YYYY-MM-DD HH:MM:SS\n" 형태 */
    if (n < 19) return;
    tmp[19] = '\0';

    strncpy(rtc_cache, tmp, sizeof(rtc_cache) - 1);
    rtc_cache[sizeof(rtc_cache) - 1] = '\0';
}

/* ========== 그래픽 ========== */
static void draw_pixel(int x, int y, int color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;

    if (color) fb[x + (y / 8) * 128] |= (1 << (y % 8));
    else       fb[x + (y / 8) * 128] &= ~(1 << (y % 8));
}

static void draw_str(int x, int y, const char *s) {
    while (*s) {
        unsigned char ch = (unsigned char)*s;
        if (ch < 32 || ch > 127) ch = '?';

        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 8; j++) {
                if (font8x8_basic[ch - 32][i] & (1 << j)) {
                    draw_pixel(x + j, y + i, 1);
                }
            }
        }
        s++;
        x += 8;
    }
}

/* ========== 상태 핸들러 ========== */
static void handle_menu(void) {
    menu_acc += (int)rotary_delta;
    rotary_delta = 0;

    while (menu_acc >= 1) {
        if (menu_index < 2) menu_index++;
        menu_acc--;
    }
    while (menu_acc <= -1) {
        if (menu_index > 0) menu_index--;
        menu_acc++;
    }

    draw_str(10, 5, "[ MENU ]");
    for (int i = 0; i < 3; i++) {
        if (i == menu_index) draw_str(5, 20 + (i * 15), ">");
        draw_str(15, 20 + (i * 15), menu_items[i]);
    }
}

static void handle_clock(void) {
    draw_str(10, 5, "[ LOCAL TIME ]");

    if (clock_mode == CLOCK_VIEW) {
        draw_str(30, 25, rtc_cache + 11); // HH:MM:SS

        char date_only[16];
        strncpy(date_only, rtc_cache, 10);
        date_only[10] = '\0';
        draw_str(20, 45, date_only);      // YYYY-MM-DD

        draw_str(5, 55, "CLICK:BACK HOLD:EDIT");
    } else {
        /* 수정 모드 */
        char buf[16];
        int blink = (time(NULL) % 2);

        if (edit_field == 0 && !blink) strcpy(buf, "    ");
        else sprintf(buf, "%04d", edit_year);
        draw_str(10, 25, buf);

        draw_str(42, 25, "-");

        if (edit_field == 1 && !blink) strcpy(buf, "  ");
        else sprintf(buf, "%02d", edit_mon);
        draw_str(50, 25, buf);

        draw_str(66, 25, "-");

        if (edit_field == 2 && !blink) strcpy(buf, "  ");
        else sprintf(buf, "%02d", edit_day);
        draw_str(74, 25, buf);

        if (edit_field == 3 && !blink) strcpy(buf, "  ");
        else sprintf(buf, "%02d", edit_hour);
        draw_str(20, 45, buf);

        draw_str(36, 45, ":");

        if (edit_field == 4 && !blink) strcpy(buf, "  ");
        else sprintf(buf, "%02d", edit_min);
        draw_str(44, 45, buf);

        draw_str(60, 45, ":");

        if (edit_field == 5 && !blink) strcpy(buf, "  ");
        else sprintf(buf, "%02d", edit_sec);
        draw_str(68, 45, buf);

        draw_str(5, 55, "CLICK:NEXT HOLD:SAVE");
    }
}

static void handle_world(void) {
    world_acc += (int)rotary_delta;
    rotary_delta = 0;

    while (world_acc >= 1) {
        world_city = (world_city + 1) % CITY_COUNT;
        world_acc--;
    }
    while (world_acc <= -1) {
        world_city = (world_city - 1 + CITY_COUNT) % CITY_COUNT;
        world_acc++;
    }

    int h = 0, m = 0, s = 0;
    if (sscanf(rtc_cache + 11, "%d:%d:%d", &h, &m, &s) != 3) {
        h = 0; m = 0; s = 0;
    }

    int hh = (h + cities[world_city].offset_hours + 24) % 24;

    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", hh, m, s);

    draw_str(10, 5, "[ WORLD CLOCK ]");

    /* 국기 태그 + 도시명 */
    draw_str(5, 28, cities[world_city].tag);
    draw_str(40, 28, cities[world_city].name);

    draw_str(35, 45, tbuf);

    draw_str(5, 55, "CLICK:BACK");
}

static void reset_game(void) {
    player_x = 60;
    obs_x = rand() % 110;
    obs_y = -10;
    score = 0;
    game_over = 0;
}

static void handle_game(void) {
    if (game_over) {
        draw_str(16, 20, "GAME OVER");
        /* 아래 문구가 잘린다고 했으니 X를 더 왼쪽(5)로 */
        draw_str(5, 42, "CLICK:RETRY");
        draw_str(5, 54, "HOLD:MENU");
        return;
    }

    /* 점수가 높아질수록 속도 증가 */
    int speed = 2 + (score / 5);   // 0~4점:2, 5~9점:3, ...
    if (speed > 10) speed = 10;

    obs_y += speed;
    if (obs_y > 64) {
        obs_y = -10;
        obs_x = rand() % 110;
        score++;
    }

    player_x += (int)(rotary_delta * 4);
    rotary_delta = 0;

    if (player_x < 0) player_x = 0;
    if (player_x > 118) player_x = 118;

    /* 충돌 */
    if (obs_y > 50 && abs(player_x - obs_x) < 10) {
        game_over = 1;
        return;
    }

    /* draw */
    for (int i = 0; i < 10; i++) draw_pixel(player_x + i, 60, 1);
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++)
            draw_pixel(obs_x + i, obs_y + j, 1);

    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "SC:%d", score);
    draw_str(0, 0, sbuf);
}

/* ========== 메인 ========== */
int main(void) {
    fd_oled = open(DEV_OLED, O_RDWR);
    fd_rot  = open(DEV_ROTARY, O_RDONLY);
    fd_rtc  = open(DEV_RTC, O_RDWR);

    if (fd_oled < 0 || fd_rot < 0 || fd_rtc < 0) {
        perror("Device Open Failed");
        return -1;
    }

    srand(time(NULL));

    fd_set fds;
    struct timeval tv;
    int synced = 0;

    reset_game();

    while (1) {
        /* 수정중이면 RTC 자동 갱신 멈추고(화면 안정), VIEW일 때만 갱신 */
        if (!(current_state == STATE_CLOCK && clock_mode == CLOCK_EDIT)) {
            poll_rtc_if_due(200);
        }

        memset(fb, 0, sizeof(fb));

        /* 입력 대기 */
        FD_ZERO(&fds);
        FD_SET(fd_rot, &fds);
        tv.tv_sec  = 0;
        tv.tv_usec = 30000;

        int r = select(fd_rot + 1, &fds, NULL, NULL, &tv);

        if (r > 0 && FD_ISSET(fd_rot, &fds)) {
            char buf[64] = {0};
            int btn = 1;

            int len = read(fd_rot, buf, sizeof(buf) - 1);
            if (len > 0) {
                buf[len] = '\0';

                if (sscanf(buf, "%ld %d", &rotary_val, &btn) >= 1) {
                    if (!synced) {
                        last_rotary_val = rotary_val;
                        synced = 1;
                    }

                    rotary_delta = rotary_val - last_rotary_val;
                    last_rotary_val = rotary_val;

                    if (btn == 0) { // press
                        if (!is_holding) {
                            clock_gettime(CLOCK_MONOTONIC, &press_start);
                            is_holding = 1;
                        }
                    } else { // release
                        if (is_holding) {
                            struct timespec now;
                            clock_gettime(CLOCK_MONOTONIC, &now);
                            long held = now.tv_sec - press_start.tv_sec;

                            if (held >= 2) {
                                /* ===== 2초 홀드 ===== */
                                if (current_state == STATE_CLOCK) {
                                    if (clock_mode == CLOCK_VIEW) {
                                        /* 수정 모드 진입 */
                                        int y,m,d,hh,mm,ss;
                                        if (sscanf(rtc_cache, "%d-%d-%d %d:%d:%d",
                                                   &y,&m,&d,&hh,&mm,&ss) == 6) {
                                            edit_year = y;
                                            edit_mon  = m;
                                            edit_day  = d;
                                            edit_hour = hh;
                                            edit_min  = mm;
                                            edit_sec  = ss;
                                        } else {
                                            edit_year=2000; edit_mon=1; edit_day=1;
                                            edit_hour=0; edit_min=0; edit_sec=0;
                                        }
                                        edit_field = 0;
                                        clock_mode = CLOCK_EDIT;
                                    } else {
                                        /* 저장 (드라이버 포맷: YY MM DD HH MM SS WD) */
                                        int yy = edit_year % 100;
                                        char cmd[64];
                                        snprintf(cmd, sizeof(cmd),
                                                 "%02d %02d %02d %02d %02d %02d 1",
                                                 yy, edit_mon, edit_day,
                                                 edit_hour, edit_min, edit_sec);
                                        write(fd_rtc, cmd, strlen(cmd));
                                        clock_mode = CLOCK_VIEW;
                                    }
                                } else if (current_state == STATE_GAME) {
                                    /* GAME: 홀드하면 메뉴 */
                                    current_state = STATE_MENU;
                                } else {
                                    /* MENU/WORLD에서는 홀드 동작 없음 */
                                }
                            } else {
                                /* ===== 짧은 클릭 ===== */
                                if (current_state == STATE_MENU) {
                                    current_state = (AppState)(menu_index + 1);
                                }
                                else if (current_state == STATE_CLOCK) {
                                    if (clock_mode == CLOCK_VIEW) {
                                        /* CLOCK VIEW: 나가기 */
                                        current_state = STATE_MENU;
                                    } else {
                                        /* CLOCK EDIT: 다음 필드 */
                                        edit_field = (edit_field + 1) % 6;
                                    }
                                }
                                else if (current_state == STATE_WORLD) {
                                    /* WORLD: 나가기 */
                                    current_state = STATE_MENU;
                                }
                                else if (current_state == STATE_GAME) {
                                    /* GAME OVER면 클릭으로 재시작 */
                                    if (game_over) reset_game();
                                }
                            }

                            is_holding = 0;
                        }
                    }
                }
            }
        }

        /* CLOCK_EDIT 상태에서 로터리로 값 변경 */
        if (current_state == STATE_CLOCK && clock_mode == CLOCK_EDIT && rotary_delta != 0) {
            long d = rotary_delta;
            rotary_delta = 0;

            switch (edit_field) {
                case 0: edit_year += (int)d; break;
                case 1: edit_mon  += (int)d; break;
                case 2: edit_day  += (int)d; break;
                case 3: edit_hour += (int)d; break;
                case 4: edit_min  += (int)d; break;
                case 5: edit_sec  += (int)d; break;
            }

            /* 최소 범위 클램프(안정) */
            if (edit_year < 2000) edit_year = 2000;
            if (edit_year > 2099) edit_year = 2099;

            if (edit_mon < 1) edit_mon = 1;
            if (edit_mon > 12) edit_mon = 12;

            if (edit_day < 1) edit_day = 1;
            if (edit_day > 31) edit_day = 31;

            if (edit_hour < 0) edit_hour = 0;
            if (edit_hour > 23) edit_hour = 23;

            if (edit_min < 0) edit_min = 0;
            if (edit_min > 59) edit_min = 59;

            if (edit_sec < 0) edit_sec = 0;
            if (edit_sec > 59) edit_sec = 59;
        }

        /* 상태별 redraw */
        switch (current_state) {
            case STATE_MENU:  handle_menu();  break;
            case STATE_CLOCK: handle_clock(); break;
            case STATE_WORLD: handle_world(); break;
            case STATE_GAME:  handle_game();  break;
            default:          handle_menu();  break;
        }

        write(fd_oled, fb, sizeof(fb));
    }

    close(fd_oled);
    close(fd_rot);
    close(fd_rtc);
    return 0;
}

