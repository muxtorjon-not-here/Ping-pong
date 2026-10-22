// Pong for Windows.
// Menu: mouse or arrow keys + Enter. Left/Right changes options.
// Player 1: W A S D (also arrow keys in 1-player mode). Player 2: arrow keys.
// Paddles move up/down AND forward/back inside their zone. Esc / P = pause.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define W 960
#define H 600
#define PW 14          // paddle width
#define PH 90          // paddle height
#define BR 9           // ball radius
#define ZONE 200       // how far a paddle can move forward from its home line
#define PSPEED 560.0f  // player paddle speed (pixels per second)
#define MAXSPEED 1400.0f
#define TRAIL 14
#define MAXPARTS 256

enum { ST_MENU, ST_COUNTDOWN, ST_PLAY, ST_PAUSE, ST_OVER };

typedef struct { COLORREF top, bottom, p1, p2, ball, text, dim; const char *name; } Theme;
static const Theme themes[] = {
    { RGB(15, 12, 40),  RGB(50, 20, 85),   RGB(0, 220, 255),   RGB(255, 60, 170),  RGB(255, 255, 255), RGB(240, 240, 255), RGB(130, 115, 180), "Neon Night" },
    { RGB(5, 35, 75),   RGB(0, 110, 140),  RGB(255, 210, 60),  RGB(255, 110, 80),  RGB(255, 255, 255), RGB(235, 250, 255), RGB(110, 175, 200), "Ocean" },
    { RGB(55, 20, 65),  RGB(220, 100, 60), RGB(255, 240, 120), RGB(120, 230, 255), RGB(255, 255, 255), RGB(255, 245, 235), RGB(215, 145, 140), "Sunset" },
    { RGB(10, 40, 20),  RGB(30, 100, 50),  RGB(180, 255, 120), RGB(255, 200, 80),  RGB(255, 255, 240), RGB(235, 255, 235), RGB(115, 170, 115), "Forest" },
    { RGB(0, 0, 0),     RGB(0, 0, 0),      RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(110, 110, 110), "Classic" },
};
#define NTHEMES 5

// react: the CPU only starts tracking once the ball passes this fraction of the field
typedef struct { const char *name; float ballSpeed, aiSpeed, aiError, react; int lunge; } Difficulty;
static const Difficulty diffs[] = {
    { "Easy",   360, 280, 70, 0.55f, 0 },
    { "Normal", 460, 400, 45, 0.40f, 0 },
    { "Hard",   560, 560, 25, 0.20f, 1 },
    { "Insane", 680, 760,  8, 0.00f, 1 },
};
#define NDIFFS 4
static const int winScores[] = { 3, 5, 7, 11 };
#define NWINS 4

typedef struct { float x, y, vx, vy, flash; } Paddle;
typedef struct { float x, y, vx, vy, life, max; COLORREF c; } Particle;

// Settings
static int mode = 0, diff = 1, winIdx = 2, theme = 0, soundOn = 1;

// Game state
static int state = ST_MENU, pausedFrom, sel, keys[256], s1, s2, rally, bestRally, serving;
static Paddle p1, p2;
static float bx, by, bvx, bvy, ballSpeed, countdown, shake, aiAim;
static float trailX[TRAIL], trailY[TRAIL];
static Particle parts[MAXPARTS];
static HWND hwnd;
static HDC back, blackDC;
static HFONT fTitle, fBig, fScore, fButton, fSmall;

#define T (themes[theme])

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(void) { return (float)rand() / RAND_MAX; }

static COLORREF lerpc(COLORREF a, COLORREF b, float t) {
    t = clampf(t, 0, 1);
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
}
static COLORREF bg_at(float y) { return lerpc(T.top, T.bottom, y / H); }

// ---------- Sound (Beep blocks, so play it on a short-lived thread) ----------

static DWORD WINAPI beep_thread(LPVOID p) {
    DWORD v = (DWORD)(UINT_PTR)p;
    Beep(v >> 16, v & 0xFFFF);
    return 0;
}
static void sound(int freq, int ms) {
    if (!soundOn) return;
    HANDLE t = CreateThread(NULL, 0, beep_thread, (LPVOID)(UINT_PTR)((freq << 16) | ms), 0, NULL);
    if (t) CloseHandle(t);
}

// ---------- Game logic ----------

static void spawn(float x, float y, COLORREF c, int n, float speed) {
    for (int i = 0; i < MAXPARTS && n > 0; i++) {
        if (parts[i].life > 0) continue;
        float a = frand() * 6.2832f, s = speed * (0.3f + frand());
        parts[i] = (Particle){ x, y, cosf(a) * s, sinf(a) * s, 0, 0.4f + frand() * 0.5f, c };
        parts[i].life = parts[i].max;
        n--;
    }
}

static void reset_trail(void) {
    for (int i = 0; i < TRAIL; i++) { trailX[i] = bx; trailY[i] = by; }
}

static void serve(int dir) {
    bx = W / 2; by = H / 2;
    ballSpeed = diffs[diff].ballSpeed;
    float a = (frand() - 0.5f) * 0.9f;
    bvx = dir * cosf(a) * ballSpeed;
    bvy = sinf(a) * ballSpeed;
    rally = 0;
    aiAim = 0;
    reset_trail();
}

static void start_game(void) {
    s1 = s2 = bestRally = 0;
    p1 = (Paddle){ 30, H / 2 - PH / 2, 0, 0, 0 };
    p2 = (Paddle){ W - 30 - PW, H / 2 - PH / 2, 0, 0, 0 };
    serve(rand() % 2 ? 1 : -1);
    state = ST_COUNTDOWN;
    countdown = 3;
    serving = 0;
    sound(600, 80);
}

static void move_paddle(Paddle *p, float dx, float dy, float minX, float maxX, float dt) {
    float ox = p->x, oy = p->y;
    p->x = clampf(p->x + dx, minX, maxX);
    p->y = clampf(p->y + dy, 0, H - PH);
    p->vx = (p->x - ox) / dt;
    p->vy = (p->y - oy) / dt;
    if (p->flash > 0) p->flash -= dt * 4;
}

static void human(Paddle *p, int up, int down, int left, int right, float minX, float maxX, float dt) {
    float dx = (float)(right - left), dy = (float)(down - up);
    if (dx && dy) { dx *= 0.7071f; dy *= 0.7071f; }
    move_paddle(p, dx * PSPEED * dt, dy * PSPEED * dt, minX, maxX, dt);
}

static void cpu(float dt) {
    const Difficulty *d = &diffs[diff];
    float home = W - 30 - PW, tx = home, ty = H / 2 - PH / 2;
    if (state == ST_PLAY && bvx > 0 && bx > W * d->react) {
        ty = by - PH / 2 + aiAim;
        if (d->lunge && W - bx < 220) tx = home - 60;  // step forward to smash
    }
    float step = d->aiSpeed * dt;
    move_paddle(&p2, clampf(tx - p2.x, -step * 0.6f, step * 0.6f), clampf(ty - p2.y, -step, step),
                W - 30 - PW - ZONE, home, dt);
}

// Returns 1 if the ball bounced off this paddle. left = paddle on the left side.
static int hit_paddle(Paddle *p, int left) {
    float cx = clampf(bx, p->x, p->x + PW), cy = clampf(by, p->y, p->y + PH);
    float dx = bx - cx, dy = by - cy;
    if (dx * dx + dy * dy > BR * BR) return 0;
    if (left ? bvx > 0 : bvx < 0) return 0;          // already moving away
    if (left ? bx < p->x : bx > p->x + PW) return 0;  // ball is behind the paddle

    float dir = left ? 1.0f : -1.0f;
    float rel = clampf((by - (p->y + PH / 2)) / (PH / 2), -1, 1);
    float push = p->vx * dir;  // moving the paddle forward into the ball hits harder
    ballSpeed = ballSpeed * 1.06f + (push > 0 ? push * 0.3f : 0);
    if (ballSpeed > MAXSPEED) ballSpeed = MAXSPEED;

    float a = rel * 1.05f;  // up to ~60 degrees off the edge of the paddle
    bvx = dir * cosf(a) * ballSpeed;
    bvy = sinf(a) * ballSpeed + p->vy * 0.15f;
    float len = sqrtf(bvx * bvx + bvy * bvy);
    bvx *= ballSpeed / len;
    bvy *= ballSpeed / len;
    if (fabsf(bvx) < ballSpeed * 0.45f) {  // never let the ball go nearly vertical
        bvx = dir * ballSpeed * 0.45f;
        bvy = (bvy < 0 ? -1 : 1) * sqrtf(ballSpeed * ballSpeed - bvx * bvx);
    }
    bx = left ? p->x + PW + BR : p->x - BR;
    p->flash = 1;
    rally++;
    if (rally > bestRally) bestRally = rally;
    spawn(bx, by, left ? T.p1 : T.p2, 14 + (int)(push > 0 ? 10 : 0), 180);
    sound(push > 100 ? 880 : 660, 25);
    return 1;
}

static void point(int toP1) {
    if (toP1) s1++; else s2++;
    spawn(toP1 ? W : 0, by, toP1 ? T.p1 : T.p2, 60, 380);
    shake = 0.35f;
    int target = winScores[winIdx];
    if (s1 >= target || s2 >= target) {
        state = ST_OVER;
        sel = 0;
        sound(toP1 || mode ? 1000 : 300, 400);
        return;
    }
    sound(toP1 ? 520 : 260, 160);
    serve(toP1 ? -1 : 1);  // serve toward whoever lost the point
    state = ST_COUNTDOWN;
    countdown = 1.2f;
    serving = 1;
}

static void update(float dt) {
    if (state == ST_PAUSE) return;

    for (int i = 0; i < MAXPARTS; i++) {
        Particle *q = &parts[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        q->x += q->vx * dt;
        q->y += q->vy * dt;
        q->vx *= 0.96f;
        q->vy *= 0.96f;
    }
    if (shake > 0) shake -= dt;

    if (state == ST_MENU) {  // demo ball bouncing behind the menu
        bx += bvx * dt; by += bvy * dt;
        if (bx < BR || bx > W - BR) { bvx = -bvx; bx = clampf(bx, BR, W - BR); }
        if (by < BR || by > H - BR) { bvy = -bvy; by = clampf(by, BR, H - BR); }
    } else if (state != ST_OVER) {
        int one = !mode;
        human(&p1, keys['W'] || (one && keys[VK_UP]), keys['S'] || (one && keys[VK_DOWN]),
              keys['A'] || (one && keys[VK_LEFT]), keys['D'] || (one && keys[VK_RIGHT]),
              30, 30 + ZONE, dt);
        if (mode) human(&p2, keys[VK_UP], keys[VK_DOWN], keys[VK_LEFT], keys[VK_RIGHT],
                        W - 30 - PW - ZONE, W - 30 - PW, dt);
        else cpu(dt);

        if (state == ST_COUNTDOWN) {
            int before = (int)ceilf(countdown);
            countdown -= dt;
            if (countdown <= 0) { state = ST_PLAY; if (!serving) sound(1000, 120); }
            else if (!serving && (int)ceilf(countdown) != before) sound(600, 80);
        } else if (state == ST_PLAY) {
            // Move in small steps so a fast ball can't pass through a paddle
            int steps = (int)(ballSpeed * dt / 5) + 1;
            for (int i = 0; i < steps && state == ST_PLAY; i++) {
                bx += bvx * dt / steps;
                by += bvy * dt / steps;
                if (by < BR)     { by = BR;     bvy = fabsf(bvy);  sound(440, 15); }
                if (by > H - BR) { by = H - BR; bvy = -fabsf(bvy); sound(440, 15); }
                if (hit_paddle(&p1, 1)) aiAim = (frand() * 2 - 1) * diffs[diff].aiError;
                hit_paddle(&p2, 0);
                if (bx < -BR * 2) point(0);
                else if (bx > W + BR * 2) point(1);
            }
        }
    }

    for (int i = TRAIL - 1; i > 0; i--) { trailX[i] = trailX[i - 1]; trailY[i] = trailY[i - 1]; }
    trailX[0] = bx; trailY[0] = by;
}

// ---------- Menus ----------

static int menu_count(void) {
    return state == ST_MENU ? 7 : state == ST_PAUSE ? 3 : state == ST_OVER ? 2 : 0;
}
static int is_option(int i) { return state == ST_MENU && i >= 1 && i <= 5; }

static RECT menu_rect(int i) {
    RECT r;
    if (state == ST_MENU) { r.left = W / 2 - 190; r.top = 205 + i * 52; r.right = W / 2 + 190; r.bottom = r.top + 42; }
    else                  { r.left = W / 2 - 150; r.top = 300 + i * 56; r.right = W / 2 + 150; r.bottom = r.top + 44; }
    return r;
}

static const char *menu_label(int i, char *buf) {
    if (state == ST_MENU) {
        switch (i) {
        case 0: return "PLAY";
        case 1: sprintf(buf, "Mode:  %s", mode ? "2 Players" : "1 Player vs CPU"); return buf;
        case 2: sprintf(buf, "Difficulty:  %s", diffs[diff].name); return buf;
        case 3: sprintf(buf, "Points to win:  %d", winScores[winIdx]); return buf;
        case 4: sprintf(buf, "Theme:  %s", T.name); return buf;
        case 5: sprintf(buf, "Sound:  %s", soundOn ? "On" : "Off"); return buf;
        default: return "QUIT";
        }
    }
    if (state == ST_PAUSE) return i == 0 ? "RESUME" : i == 1 ? "RESTART" : "MAIN MENU";
    return i == 0 ? "PLAY AGAIN" : "MAIN MENU";
}

static void go_menu(void) {
    state = ST_MENU;
    sel = 0;
    serve(1);
    bvy = ballSpeed * 0.6f;
}

// dir: 0 = Enter/click, -1/+1 = Left/Right on an option
static void menu_activate(int i, int dir) {
    int step = dir < 0 ? -1 : 1;
    if (state == ST_MENU) {
        switch (i) {
        case 0: if (!dir) start_game(); return;
        case 1: mode = !mode; break;
        case 2: diff = (diff + step + NDIFFS) % NDIFFS; break;
        case 3: winIdx = (winIdx + step + NWINS) % NWINS; break;
        case 4: theme = (theme + step + NTHEMES) % NTHEMES; break;
        case 5: soundOn = !soundOn; break;
        case 6: if (!dir) PostMessage(hwnd, WM_CLOSE, 0, 0); return;
        }
        sound(750, 30);
    } else if (state == ST_PAUSE && !dir) {
        if (i == 0) state = pausedFrom;
        else if (i == 1) start_game();
        else go_menu();
    } else if (state == ST_OVER && !dir) {
        if (i == 0) start_game(); else go_menu();
    }
}

static int menu_hit(int x, int y) {
    for (int i = 0; i < menu_count(); i++) {
        RECT r = menu_rect(i);
        POINT pt = { x, y };
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static void pause_game(void) {
    if (state != ST_PLAY && state != ST_COUNTDOWN) return;
    pausedFrom = state;
    state = ST_PAUSE;
    sel = 0;
}

// ---------- Drawing ----------

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void round_rect(HDC dc, int x, int y, int w, int h, int rad, COLORREF fillc, COLORREF border) {
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fillc);
    SetDCPenColor(dc, border);
    RoundRect(dc, x, y, x + w, y + h, rad, rad);
}

static void circle(HDC dc, float cx, float cy, float r, COLORREF c) {
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c);
    SetDCPenColor(dc, c);
    Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r) + 1, (int)(cy + r) + 1);
}

static void text(HDC dc, HFONT f, const char *s, int x, int y, int w, int h, COLORREF c) {
    RECT r = { x, y, x + w, y + h };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void shadow_text(HDC dc, HFONT f, const char *s, int x, int y, int w, int h, COLORREF c, COLORREF sh, int off) {
    text(dc, f, s, x + off, y + off, w, h, sh);
    text(dc, f, s, x, y, w, h, c);
}

static void darken(HDC dc, int alpha) {
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)alpha, 0 };
    AlphaBlend(dc, 0, 0, W, H, blackDC, 0, 0, 1, 1, bf);
}

static void draw_paddle(HDC dc, Paddle *p, COLORREF c) {
    COLORREF bg = bg_at(p->y + PH / 2);
    COLORREF col = lerpc(c, RGB(255, 255, 255), p->flash);
    round_rect(dc, (int)p->x - 5, (int)p->y - 5, PW + 10, PH + 10, 16, lerpc(bg, c, 0.25f), lerpc(bg, c, 0.25f));
    round_rect(dc, (int)p->x, (int)p->y, PW, PH, 10, col, col);
}

static void draw_menu_buttons(HDC dc) {
    char buf[64];
    for (int i = 0; i < menu_count(); i++) {
        RECT r = menu_rect(i);
        int on = i == sel;
        COLORREF base = lerpc(T.top, RGB(255, 255, 255), 0.10f);
        round_rect(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 18,
                   on ? T.p1 : base, on ? T.p1 : T.dim);
        COLORREF tc = on ? RGB(20, 20, 30) : T.text;
        text(dc, fButton, menu_label(i, buf), r.left, r.top, r.right - r.left, r.bottom - r.top, tc);
        if (on && is_option(i)) {
            text(dc, fButton, "<", r.left + 6, r.top, 30, r.bottom - r.top, tc);
            text(dc, fButton, ">", r.right - 36, r.top, 30, r.bottom - r.top, tc);
        }
    }
}

static void render(void) {
    HDC dc = back;
    SetBkMode(dc, TRANSPARENT);
    char buf[96];

    for (int y = 0; y < H; y += 4) fill(dc, 0, y, W, 4, bg_at((float)y));

    int ox = 0, oy = 0;
    if (shake > 0) { ox = (int)((frand() - 0.5f) * shake * 40); oy = (int)((frand() - 0.5f) * shake * 40); }
    SetViewportOrgEx(dc, ox, oy, NULL);

    // Court
    COLORREF line = lerpc(T.top, T.dim, 0.6f);
    for (int y = 10; y < H; y += 34) fill(dc, W / 2 - 2, y, 4, 18, line);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, line);
    Ellipse(dc, W / 2 - 70, H / 2 - 70, W / 2 + 70, H / 2 + 70);
    COLORREF zone = lerpc(T.top, T.dim, 0.3f);
    for (int y = 0; y < H; y += 12) {
        fill(dc, 30 + ZONE + PW, y, 2, 5, zone);
        fill(dc, W - 30 - PW - ZONE - 2, y, 2, 5, zone);
    }

    if (state != ST_MENU) {
        draw_paddle(dc, &p1, T.p1);
        draw_paddle(dc, &p2, T.p2);
    }

    // Ball trail, then ball
    for (int i = TRAIL - 1; i > 0; i--) {
        float t = 1.0f - (float)i / TRAIL;
        circle(dc, trailX[i], trailY[i], BR * (0.3f + 0.7f * t), lerpc(bg_at(trailY[i]), T.p1, t * 0.6f));
    }
    circle(dc, bx, by, BR + 3, lerpc(bg_at(by), T.ball, 0.35f));
    circle(dc, bx, by, BR, T.ball);

    for (int i = 0; i < MAXPARTS; i++) {
        Particle *q = &parts[i];
        if (q->life <= 0) continue;
        int sz = 2 + (int)(4 * q->life / q->max);
        fill(dc, (int)q->x - sz / 2, (int)q->y - sz / 2, sz, sz, lerpc(bg_at(q->y), q->c, q->life / q->max));
    }
    SetViewportOrgEx(dc, 0, 0, NULL);

    if (state == ST_MENU) {
        darken(dc, 110);
        shadow_text(dc, fTitle, "PONG", 0, 40, W, 120, T.p1, T.p2, 5);
        text(dc, fSmall, "Mouse or arrow keys + Enter.  Left / Right to change options.", 0, 160, W, 30, T.dim);
        draw_menu_buttons(dc);
        text(dc, fSmall, "P1: W A S D      P2: Arrow keys      Esc: pause", 0, 570, W, 26, T.dim);
        return;
    }

    // HUD
    sprintf(buf, "%d", s1);
    shadow_text(dc, fScore, buf, W / 2 - 160, 14, 120, 80, T.p1, RGB(0, 0, 0), 3);
    sprintf(buf, "%d", s2);
    shadow_text(dc, fScore, buf, W / 2 + 40, 14, 120, 80, T.p2, RGB(0, 0, 0), 3);
    text(dc, fSmall, "PLAYER 1", W / 2 - 160, 90, 120, 22, T.dim);
    text(dc, fSmall, mode ? "PLAYER 2" : "CPU", W / 2 + 40, 90, 120, 22, T.dim);
    if (rally > 1) {
        sprintf(buf, "Rally %d", rally);
        text(dc, fSmall, buf, 0, 116, W, 22, T.text);
    }
    sprintf(buf, "%s   |   First to %d   |   Esc = pause", diffs[diff].name, winScores[winIdx]);
    text(dc, fSmall, buf, 0, H - 30, W, 24, T.dim);

    if (state == ST_COUNTDOWN || (state == ST_PAUSE && pausedFrom == ST_COUNTDOWN)) {
        if (serving) strcpy(buf, "Get ready");
        else sprintf(buf, "%d", (int)ceilf(countdown));
        shadow_text(dc, serving ? fBig : fTitle, buf, 0, H / 2 - 150, W, 120, T.text, RGB(0, 0, 0), 4);
    }

    if (state == ST_PAUSE) {
        darken(dc, 150);
        shadow_text(dc, fBig, "PAUSED", 0, 190, W, 80, T.text, RGB(0, 0, 0), 4);
        draw_menu_buttons(dc);
    } else if (state == ST_OVER) {
        darken(dc, 150);
        int p1won = s1 > s2;
        shadow_text(dc, fBig, p1won ? "PLAYER 1 WINS!" : mode ? "PLAYER 2 WINS!" : "CPU WINS",
                    0, 150, W, 80, p1won ? T.p1 : T.p2, RGB(0, 0, 0), 4);
        sprintf(buf, "Final score  %d - %d      Longest rally: %d", s1, s2, bestRally);
        text(dc, fButton, buf, 0, 235, W, 40, T.text);
        draw_menu_buttons(dc);
    }
}

static void present(void) {
    render();
    HDC dc = GetDC(hwnd);
    BitBlt(dc, 0, 0, W, H, back, 0, 0, SRCCOPY);
    ReleaseDC(hwnd, dc);
}

// ---------- Window ----------

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int n = menu_count();
    switch (msg) {
    case WM_KEYDOWN:
        if (wp < 256) keys[wp] = 1;
        if (n) {
            if (wp == VK_UP || wp == 'W')        { sel = (sel - 1 + n) % n; sound(500, 15); }
            else if (wp == VK_DOWN || wp == 'S') { sel = (sel + 1) % n; sound(500, 15); }
            else if (wp == VK_LEFT || wp == 'A') menu_activate(sel, -1);
            else if (wp == VK_RIGHT || wp == 'D') menu_activate(sel, 1);
            else if (wp == VK_RETURN || wp == VK_SPACE) menu_activate(sel, 0);
            else if (wp == VK_ESCAPE) {
                if (state == ST_PAUSE) state = pausedFrom;
                else if (state == ST_OVER) go_menu();
                else PostMessage(h, WM_CLOSE, 0, 0);
            }
        } else if (wp == VK_ESCAPE || wp == 'P') {
            pause_game();
        }
        return 0;
    case WM_KEYUP:
        if (wp < 256) keys[wp] = 0;
        return 0;
    case WM_MOUSEMOVE: {
        int i = menu_hit((short)LOWORD(lp), (short)HIWORD(lp));
        if (i >= 0 && i != sel) { sel = i; sound(500, 10); }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int i = menu_hit((short)LOWORD(lp), (short)HIWORD(lp));
        if (i >= 0) { sel = i; menu_activate(i, is_option(i) ? (msg == WM_RBUTTONDOWN ? -1 : 1) : 0); }
        return 0;
    }
    case WM_KILLFOCUS:
        ZeroMemory(keys, sizeof(keys));
        pause_game();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BitBlt(dc, 0, 0, W, H, back, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static HFONT font(int size, int weight, const char *face) {
    return CreateFontA(size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    srand(GetTickCount());

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "PongWindow";
    RegisterClassA(&wc);

    // Size the window so the drawable area is exactly W x H
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, style, FALSE);
    hwnd = CreateWindowA("PongWindow", "Pong", style, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);

    // Off-screen buffer we draw everything into, then copy to the window (no flicker)
    HDC wdc = GetDC(hwnd);
    back = CreateCompatibleDC(wdc);
    SelectObject(back, CreateCompatibleBitmap(wdc, W, H));
    blackDC = CreateCompatibleDC(wdc);
    SelectObject(blackDC, CreateCompatibleBitmap(wdc, 1, 1));
    SetPixel(blackDC, 0, 0, RGB(0, 0, 0));
    ReleaseDC(hwnd, wdc);

    fTitle  = font(120, FW_BLACK, "Segoe UI");
    fBig    = font(64, FW_BOLD, "Segoe UI");
    fScore  = font(76, FW_BOLD, "Consolas");
    fButton = font(24, FW_SEMIBOLD, "Segoe UI");
    fSmall  = font(18, FW_NORMAL, "Segoe UI");

    go_menu();
    ShowWindow(hwnd, show);

    timeBeginPeriod(1);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    const double frame = 1.0 / 120;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { timeEndPeriod(1); return 0; }
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - last.QuadPart) / freq.QuadPart;
        if (dt < frame) { Sleep(1); continue; }  // cap at ~120 frames per second
        last = now;
        update((float)(dt > 0.05 ? 0.05 : dt));
        if (!IsIconic(hwnd)) present();
    }
}
