#pragma once
#include <cmath>

struct Mat3 {
	float m[3][3];

	void Identity() {
		m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
		m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
		m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
	}

	void RotateX(float rad) {
		float s = sinf(rad), c = cosf(rad);
		m[0][0] = 1; m[0][1] = 0; m[0][2] = 0;
		m[1][0] = 0; m[1][1] = c; m[1][2] = s;
		m[2][0] = 0; m[2][1] = -s; m[2][2] = c;
	}

	void RotateY(float rad) {
		float s = sinf(rad), c = cosf(rad);
		m[0][0] = c; m[0][1] = 0; m[0][2] = -s;
		m[1][0] = 0; m[1][1] = 1; m[1][2] = 0;
		m[2][0] = s; m[2][1] = 0; m[2][2] = c;
	}

	void RotateZ(float rad) {
		float s = sinf(rad), c = cosf(rad);
		m[0][0] = c; m[0][1] = s; m[0][2] = 0;
		m[1][0] = -s; m[1][1] = c; m[1][2] = 0;
		m[2][0] = 0; m[2][1] = 0; m[2][2] = 1;
	}

	Mat3 operator*(const Mat3& b) const {
		Mat3 r;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				r.m[i][j] = m[i][0]*b.m[0][j] + m[i][1]*b.m[1][j] + m[i][2]*b.m[2][j];
		return r;
	}
};
