"""
Magic wand visualizer.
Reads serial data from the Arduino wand and plays particle animations.
No color sensor: the animation color is chosen with the keyboard.

Usage:
    python sim.py                 # auto-detect Arduino
    python sim.py --port COM5     # or /dev/ttyACM0
    python sim.py --demo          # no hardware: keyboard simulation

Keys (always active):   R/G/B/Y/W set animation color,  C cycle colors,  ESC quit
Demo-only keys: 1 SWISH_L  2 SWISH_R  3 FLICK_UP  4 FLICK_DOWN
                5 SHAKE    6 TWIRL    7 THRUST
                9 PROX NEAR   0 PROX FAR
                SPACE toggle sleep
"""

import argparse
import math
import random
import sys
import threading
import time

import pygame
import serial
import serial.tools.list_ports

W, H = 1000, 650
BAUD = 9600
WARN = (255, 60, 60)

PALETTE = [
    (255, 255, 255),
    (255, 40, 40),
    (255, 150, 30),
    (255, 220, 40),
    (40, 255, 60),
    (60, 100, 255),
    (180, 70, 255),
]


# ------------------------------------------------------------------ serial
class WandLink:
    def __init__(self, port):
        self.awake = True
        self.color = PALETTE[0]
        self.prox_near = False
        self.events = []          # queue of event names
        self.lock = threading.Lock()
        self.running = True
        self.ser = serial.Serial(port, BAUD, timeout=0.2) if port else None
        if self.ser:
            threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        time.sleep(2)  # Uno resets on serial open
        while self.running:
            try:
                line = self.ser.readline().decode("ascii", errors="ignore").strip()
            except serial.SerialException:
                break
            if line:
                self._parse(line)

    def _parse(self, line):
        if line.startswith("#"):      # human log lines from the firmware
            return
        p = line.split(",")
        try:
            with self.lock:
                if p[0] == "S":
                    self.awake = p[1] == "1"
                    if not self.awake:
                        self.prox_near = False
                elif p[0] == "G":
                    self.events.append(p[1])
                elif p[0] == "P":
                    if p[1] == "NEAR":
                        self.prox_near = True
                    elif p[1] == "FAR":
                        self.prox_near = False
                    self.events.append("PROX_" + p[1])
        except (ValueError, IndexError):
            pass

    def pop_events(self):
        with self.lock:
            ev, self.events = self.events, []
        return ev

    def push(self, name):
        with self.lock:
            self.events.append(name)


def find_port():
    for p in serial.tools.list_ports.comports():
        d = (p.description + " " + (p.manufacturer or "")).lower()
        if any(k in d for k in ("arduino", "ch340", "usb serial", "acm")):
            return p.device
    return None


# --------------------------------------------------------------- particles
class Particle:
    __slots__ = ("x", "y", "vx", "vy", "life", "max_life", "size", "color", "gravity", "drag")

    def __init__(self, x, y, vx, vy, life, size, color, gravity=0.0, drag=0.99):
        self.x, self.y, self.vx, self.vy = x, y, vx, vy
        self.life = self.max_life = life
        self.size, self.color = size, color
        self.gravity, self.drag = gravity, drag

    def update(self, dt):
        self.vx *= self.drag
        self.vy = self.vy * self.drag + self.gravity * dt
        self.x += self.vx * dt
        self.y += self.vy * dt
        self.life -= dt
        return self.life > 0

    def draw(self, surf):
        t = max(0.0, self.life / self.max_life)
        r = max(1, int(self.size * t))
        col = tuple(int(c * t) for c in self.color)
        pygame.draw.circle(surf, col, (int(self.x), int(self.y)), r)


class Ring:
    def __init__(self, x, y, color):
        self.x, self.y, self.color = x, y, color
        self.r, self.life = 10, 0.8
        self.max_life = 0.8

    def update(self, dt):
        self.r += 700 * dt
        self.life -= dt
        return self.life > 0

    def draw(self, surf):
        t = max(0.0, self.life / self.max_life)
        col = tuple(int(c * t) for c in self.color)
        pygame.draw.circle(surf, col, (int(self.x), int(self.y)), int(self.r), max(1, int(8 * t)))


class Spiral:
    """Emits particles along an expanding spiral for a short time."""
    def __init__(self, cx, cy, color, world):
        self.cx, self.cy, self.color, self.world = cx, cy, color, world
        self.t, self.dur = 0.0, 1.4

    def update(self, dt):
        self.t += dt
        for k in range(3):
            a = (self.t * 9) + k * (2 * math.pi / 3)
            r = 30 + self.t * 180
            x = self.cx + math.cos(a) * r
            y = self.cy + math.sin(a) * r
            self.world.append(Particle(x, y, math.cos(a + 1.57) * 40, math.sin(a + 1.57) * 40,
                                       0.8, 7, self.color, drag=0.96))
        return self.t < self.dur

    def draw(self, surf):
        pass


# --------------------------------------------------------------- animations
def vary(color, amt=40):
    return tuple(max(0, min(255, c + random.randint(-amt, amt))) for c in color)


def spawn(gesture, color, world, fx):
    cx, cy = W // 2, H // 2
    if gesture in ("SWISH_R", "SWISH_L"):
        d = 1 if gesture == "SWISH_R" else -1
        x0 = 60 if d > 0 else W - 60
        for i in range(160):
            x = x0 + d * i * (W - 120) / 160
            y = cy + math.sin(i * 0.12) * 60
            world.append(Particle(x, y + random.uniform(-10, 10),
                                  random.uniform(-30, 30), random.uniform(-60, 60),
                                  random.uniform(0.5, 1.2), random.uniform(4, 9),
                                  vary(color), drag=0.95))
    elif gesture in ("FLICK_UP", "FLICK_DOWN"):
        d = -1 if gesture == "FLICK_UP" else 1
        y0 = H - 40 if d < 0 else 40
        for _ in range(220):
            world.append(Particle(cx + random.gauss(0, 40), y0,
                                  random.gauss(0, 90), d * random.uniform(300, 900),
                                  random.uniform(0.6, 1.4), random.uniform(3, 8),
                                  vary(color), gravity=-d * -500 * 0.4, drag=0.97))
    elif gesture == "SHAKE":
        for _ in range(500):
            a = random.uniform(0, 2 * math.pi)
            s = random.uniform(100, 700)
            world.append(Particle(random.uniform(0, W), random.uniform(0, H),
                                  math.cos(a) * s, math.sin(a) * s,
                                  random.uniform(0.4, 1.3), random.uniform(2, 6),
                                  vary(color, 90), drag=0.94))
    elif gesture == "TWIRL":
        fx.append(Spiral(cx, cy, color, world))
    elif gesture == "THRUST":
        fx.append(Ring(cx, cy, color))
        for _ in range(120):
            a = random.uniform(0, 2 * math.pi)
            s = random.uniform(200, 600)
            world.append(Particle(cx, cy, math.cos(a) * s, math.sin(a) * s,
                                  random.uniform(0.3, 0.9), random.uniform(3, 6),
                                  vary(color), drag=0.93))
    elif gesture == "PROX_NEAR":
        fx.append(Ring(cx, cy, WARN))
        for _ in range(180):
            a = random.uniform(0, 2 * math.pi)
            s = random.uniform(250, 750)
            world.append(Particle(cx, cy, math.cos(a) * s, math.sin(a) * s,
                                  random.uniform(0.4, 1.0), random.uniform(3, 7),
                                  vary(WARN, 30), drag=0.94))
    # PROX_FAR has no particle effect, only a label (handled in main)


# --------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--demo", action="store_true")
    args = ap.parse_args()

    port = None if args.demo else (args.port or find_port())
    if not args.demo and not port:
        print("No Arduino found. Use --port or --demo. Starting demo mode.")
    link = WandLink(port)
    demo = port is None
    if port:
        print("Connected on", port)

    pygame.init()
    screen = pygame.display.set_mode((W, H))
    pygame.display.set_caption("Magic Wand")
    clock = pygame.time.Clock()
    font = pygame.font.SysFont("consolas", 22)
    big = pygame.font.SysFont("consolas", 64, bold=True)
    fade = pygame.Surface((W, H))
    fade.fill((0, 0, 0))

    particles, fx = [], []
    last_label, label_t = "", 0.0
    sleep_alpha = 0.0
    warn_pulse = 0.0
    palette_idx = 0
    demo_keys = {
        pygame.K_1: "SWISH_L", pygame.K_2: "SWISH_R", pygame.K_3: "FLICK_UP",
        pygame.K_4: "FLICK_DOWN", pygame.K_5: "SHAKE", pygame.K_6: "TWIRL",
        pygame.K_7: "THRUST",
    }
    color_keys = {
        pygame.K_r: (255, 40, 40), pygame.K_g: (40, 255, 60), pygame.K_b: (60, 100, 255),
        pygame.K_y: (255, 220, 40), pygame.K_w: (255, 255, 255),
    }

    running = True
    while running:
        dt = clock.tick(60) / 1000.0

        for e in pygame.event.get():
            if e.type == pygame.QUIT:
                running = False
            elif e.type == pygame.KEYDOWN:
                if e.key == pygame.K_ESCAPE:
                    running = False
                # color selection works with real hardware too
                if e.key in color_keys:
                    link.color = color_keys[e.key]
                elif e.key == pygame.K_c:
                    palette_idx = (palette_idx + 1) % len(PALETTE)
                    link.color = PALETTE[palette_idx]
                if demo:
                    if e.key in demo_keys:
                        link.push(demo_keys[e.key])
                    elif e.key == pygame.K_9:
                        link.prox_near = True
                        link.push("PROX_NEAR")
                    elif e.key == pygame.K_0:
                        link.prox_near = False
                        link.push("PROX_FAR")
                    elif e.key == pygame.K_SPACE:
                        link.awake = not link.awake
                        if not link.awake:
                            link.prox_near = False

        for g in link.pop_events():
            if link.awake:
                spawn(g, link.color, particles, fx)
                if g == "PROX_FAR":
                    last_label = "PATH CLEAR"
                elif g == "PROX_NEAR":
                    last_label = "OBJECT NEAR"
                else:
                    last_label = g.replace("_", " ")
                label_t = 1.2

        # trails
        fade.set_alpha(45)
        screen.blit(fade, (0, 0))

        particles = [p for p in particles if p.update(dt)]
        fx = [f for f in fx if f.update(dt)]
        for p in particles:
            p.draw(screen)
        for f in fx:
            f.draw(screen)

        # red pulsing border while an object is near
        if link.prox_near and link.awake:
            warn_pulse += dt * 6
            k = 0.55 + 0.45 * math.sin(warn_pulse)
            col = (int(WARN[0] * k), int(WARN[1] * k), int(WARN[2] * k))
            pygame.draw.rect(screen, col, (0, 0, W, H), 10)
        else:
            warn_pulse = 0.0

        # HUD: color swatch and state
        pygame.draw.rect(screen, link.color, (20, 20, 40, 40), border_radius=8)
        pygame.draw.rect(screen, (90, 90, 90), (20, 20, 40, 40), 2, border_radius=8)
        status = "AWAKE" if link.awake else "SLEEPING"
        screen.blit(font.render(status, True, (200, 200, 200)), (75, 28))
        if link.awake and link.prox_near:
            screen.blit(font.render("OBJECT NEAR", True, WARN), (75, 56))
        if demo:
            hint = "DEMO  1-7 gestures  9 near  0 far  SPACE sleep  R/G/B/Y/W/C color"
        else:
            hint = "R/G/B/Y/W/C change animation color"
        screen.blit(font.render(hint, True, (120, 120, 120)), (20, H - 32))

        if label_t > 0:
            label_t -= dt
            lab_col = WARN if last_label == "OBJECT NEAR" else link.color
            txt = big.render(last_label, True, lab_col)
            txt.set_alpha(int(255 * min(1, label_t / 0.6)))
            screen.blit(txt, txt.get_rect(center=(W // 2, 70)))

        # sleep overlay
        target = 0.0 if link.awake else 200.0
        sleep_alpha += (target - sleep_alpha) * min(1, dt * 4)
        if sleep_alpha > 1:
            ov = pygame.Surface((W, H))
            ov.set_alpha(int(sleep_alpha))
            screen.blit(ov, (0, 0))
            z = big.render("z z z", True, (90, 90, 140))
            z.set_alpha(int(sleep_alpha))
            screen.blit(z, z.get_rect(center=(W // 2, H // 2)))

        pygame.display.flip()

    link.running = False
    pygame.quit()
    sys.exit()


if __name__ == "__main__":
    main()