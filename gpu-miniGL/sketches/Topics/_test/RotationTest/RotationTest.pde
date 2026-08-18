/**
 * Rotation bring-up: SOLID colours only.
 *
 * A flat fill is immune to every geometry error -- pitch, stride, base
 * address, rotation angle. If the LTDC rotation datapath works at all, this
 * must show clean solid colours cycling red -> green -> blue -> white.
 * Anything else (noise, flashing, black) means the problem is the datapath
 * itself, not how the image is laid out.
 */
int phase = 0;
int lastSwitch = 0;

void setup() {
    size(640, 360);
}

void draw() {
    if (millis() - lastSwitch > 2000) {
        lastSwitch = millis();
        phase = (phase + 1) % 4;
    }
    if (phase == 0) background(255, 0, 0);
    else if (phase == 1) background(0, 255, 0);
    else if (phase == 2) background(0, 0, 255);
    else background(255, 255, 255);
}
