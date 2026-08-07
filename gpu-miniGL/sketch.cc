#include "psketch.hh"

// =============================================================================
//  sketch.cc -- "Bouncy Bubbles", a real Processing example, hand-translated
// =============================================================================
//
// Source: the Processing example Topics/Motion/BouncyBubbles ("based on code
// from Keith Peters. Multiple-object collision."), translated line-for-line
// from the .pde. https://processing.org/examples/bouncybubbles.html
//
// Translation notes, the complete list:
//  - Java class -> struct; the `others` array reference is just the global.
//  - atan2 -> the sketch only ever takes cos/sin of that angle, which are
//    dx/distance and dy/distance -- so those are used directly (mgl_math has
//    no atan2, and this is exact rather than approximate).
//  - the original is sized 640x360; diameters scale by width/640 and gravity
//    by height/360 so it plays the same on any panel.

namespace
{

constexpr int numBalls = 12;
constexpr float spring = 0.05f;
float gravity = 0.03f; // scaled for panel height in sketch_setup()
constexpr float friction = -0.9f;

struct Ball {
	float x = 0, y = 0;
	float diameter = 1;
	float vx = 0, vy = 0;
	int id = 0;

	void collide();
	void move();
	void display();
};

Ball balls[numBalls];

void Ball::collide()
{
	for (int i = id + 1; i < numBalls; i++) {
		const float dx = balls[i].x - x;
		const float dy = balls[i].y - y;
		const float distance = sqrt(dx * dx + dy * dy);
		const float minDist = balls[i].diameter / 2 + diameter / 2;
		if (distance < minDist && distance > 0.0001f) {
			// cos(atan2(dy,dx)) == dx/distance, sin == dy/distance
			const float targetX = x + (dx / distance) * minDist;
			const float targetY = y + (dy / distance) * minDist;
			const float ax = (targetX - balls[i].x) * spring;
			const float ay = (targetY - balls[i].y) * spring;
			vx -= ax;
			vy -= ay;
			balls[i].vx += ax;
			balls[i].vy += ay;
		}
	}
}

void Ball::move()
{
	vy += gravity;
	x += vx;
	y += vy;
	if (x + diameter / 2 > width) {
		x = width - diameter / 2;
		vx *= friction;
	} else if (x - diameter / 2 < 0) {
		x = diameter / 2;
		vx *= friction;
	}
	if (y + diameter / 2 > height) {
		y = height - diameter / 2;
		vy *= friction;
	} else if (y - diameter / 2 < 0) {
		y = diameter / 2;
		vy *= friction;
	}
}

void Ball::display()
{
	ellipse(x, y, diameter, diameter);
}

} // namespace

void sketch_setup()
{
	const float s = float(width) / 640.0f; // original sketch is size(640, 360)
	gravity = 0.03f * (float(height) / 360.0f);
	for (int i = 0; i < numBalls; i++)
		balls[i] = {random(width), random(height), random(30, 70) * s, 0, 0, i};
	noStroke();
	fill(255, 204);
}

void sketch_draw()
{
	background(0);
	for (Ball &ball : balls) {
		ball.collide();
		ball.move();
		ball.display();
	}
}
