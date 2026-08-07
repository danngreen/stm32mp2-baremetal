/**
 * Bouncy Bubbles
 * based on code from Keith Peters.
 *
 * Multiple-object collision.
 *
 * (from the Processing examples, Topics/Motion/BouncyBubbles; the Java class
 * became a struct -- defined before use, as C++ wants -- and ball sizes /
 * gravity scale by the panel size, since the original is size(640, 360))
 */

const int numBalls = 12;
float spring = 0.05;
float gravity = 0.03; // scaled for panel height in setup()
float friction = -0.9;

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
		float dx = balls[i].x - x;
		float dy = balls[i].y - y;
		float distance = sqrt(dx * dx + dy * dy);
		float minDist = balls[i].diameter / 2 + diameter / 2;
		if (distance < minDist) {
			float angle = atan2(dy, dx);
			float targetX = x + cos(angle) * minDist;
			float targetY = y + sin(angle) * minDist;
			float ax = (targetX - balls[i].x) * spring;
			float ay = (targetY - balls[i].y) * spring;
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

void setup()
{
	size(640, 360);
	const float s = width / 640.0f;
	gravity = 0.03f * (height / 360.0f);
	for (int i = 0; i < numBalls; i++)
		balls[i] = {random(width), random(height), random(30, 70) * s, 0, 0, i};
	noStroke();
	fill(255, 204);
}

void draw()
{
	background(0);
	for (Ball &ball : balls) {
		ball.collide();
		ball.move();
		ball.display();
	}
}
