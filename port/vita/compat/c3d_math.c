// citro3d's matrix helpers, for the Vita build. Same conventions: row-major C3D_Mtx, vectors
// multiplied on the right, "bRightSide" meaning m = m * op, and the PICA's projections (depth range
// [-1, 0], the Tilt variants turned a quarter for the 3DS's sideways screens). The GL side undoes
// the depth range and the turn (c3d_gl.c), so the game's matrices stay exactly what they were.
#include <citro3d.h>

void Mtx_Identity(C3D_Mtx* m)
{
	Mtx_Zeros(m);
	m->r[0].x = m->r[1].y = m->r[2].z = m->r[3].w = 1.0f;
}

void Mtx_Multiply(C3D_Mtx* out, const C3D_Mtx* a, const C3D_Mtx* b)
{
	C3D_Mtx t;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += a->r[i].c[3 - k] * b->r[k].c[3 - j];
			t.r[i].c[3 - j] = s;
		}
	*out = t;
}

static inline float dot4(C3D_FVec a, C3D_FVec b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

C3D_FVec Mtx_MultiplyFVec4(const C3D_Mtx* m, C3D_FVec v)
{
	return FVec4_New(dot4(m->r[0], v), dot4(m->r[1], v), dot4(m->r[2], v), dot4(m->r[3], v));
}

C3D_FVec Mtx_MultiplyFVec3(const C3D_Mtx* m, C3D_FVec v)
{
	return FVec3_New(FVec3_Dot(m->r[0], v), FVec3_Dot(m->r[1], v), FVec3_Dot(m->r[2], v));
}

static void apply(C3D_Mtx* m, const C3D_Mtx* op, bool right)
{
	if (right)
		Mtx_Multiply(m, m, op);
	else
		Mtx_Multiply(m, op, m);
}

void Mtx_Translate(C3D_Mtx* m, float x, float y, float z, bool bRightSide)
{
	C3D_Mtx t;
	Mtx_Identity(&t);
	t.r[0].w = x;
	t.r[1].w = y;
	t.r[2].w = z;
	apply(m, &t, bRightSide);
}

void Mtx_Scale(C3D_Mtx* m, float x, float y, float z)
{
	for (int i = 0; i < 4; i++) {
		m->r[i].x *= x;
		m->r[i].y *= y;
		m->r[i].z *= z;
	}
}

static void rotate(C3D_Mtx* m, int axis, float angle, bool right)
{
	float s = sinf(angle), c = cosf(angle);
	C3D_Mtx r;
	Mtx_Identity(&r);
	switch (axis) {
	case 0: r.r[1].y = c; r.r[1].z = -s; r.r[2].y = s; r.r[2].z = c; break;
	case 1: r.r[0].x = c; r.r[0].z = s; r.r[2].x = -s; r.r[2].z = c; break;
	default: r.r[0].x = c; r.r[0].y = -s; r.r[1].x = s; r.r[1].y = c; break;
	}
	apply(m, &r, right);
}

void Mtx_RotateX(C3D_Mtx* m, float a, bool r) { rotate(m, 0, a, r); }
void Mtx_RotateY(C3D_Mtx* m, float a, bool r) { rotate(m, 1, a, r); }
void Mtx_RotateZ(C3D_Mtx* m, float a, bool r) { rotate(m, 2, a, r); }

void Mtx_Ortho(C3D_Mtx* m, float l, float r, float b, float t, float n, float f, bool lh)
{
	Mtx_Zeros(m);
	m->r[0].x = 2.0f / (r - l);
	m->r[0].w = (l + r) / (l - r);
	m->r[1].y = 2.0f / (t - b);
	m->r[1].w = (b + t) / (b - t);
	m->r[2].z = lh ? 1.0f / (f - n) : 1.0f / (n - f);
	m->r[2].w = 0.5f * (n + f) / (n - f) - 0.5f;
	m->r[3].w = 1.0f;
}

// The tilted ortho: the plain one turned a quarter, so screen y runs along the PICA's x
void Mtx_OrthoTilt(C3D_Mtx* m, float l, float r, float b, float t, float n, float f, bool lh)
{
	Mtx_Zeros(m);
	m->r[0].y = 2.0f / (t - b);
	m->r[0].w = (b + t) / (b - t);
	m->r[1].x = 2.0f / (l - r);
	m->r[1].w = (l + r) / (r - l);
	m->r[2].z = lh ? 1.0f / (f - n) : 1.0f / (n - f);
	m->r[2].w = 0.5f * (n + f) / (n - f) - 0.5f;
	m->r[3].w = 1.0f;
}

void Mtx_Persp(C3D_Mtx* m, float fovy, float aspect, float n, float f, bool lh)
{
	float tn = tanf(fovy * 0.5f);
	Mtx_Zeros(m);
	m->r[0].x = 1.0f / (aspect * tn);
	m->r[1].y = 1.0f / tn;
	m->r[3].z = lh ? 1.0f : -1.0f;
	m->r[2].z = -m->r[3].z * n / (n - f);
	m->r[2].w = f * n / (n - f);
}

// citro3d's Tilt projections take what the sideways screen makes of fovy and the aspect: the
// horizontal field of view and the inverse aspect ratio
void Mtx_PerspStereoTilt(C3D_Mtx* m, float fovx, float invaspect, float n, float f, float iod, float screen, bool lh)
{
	float tx = tanf(fovx * 0.5f);
	float ty = tx * invaspect;
	float shift = screen != 0.0f ? iod / (2.0f * screen) : 0.0f;
	Mtx_Zeros(m);
	m->r[3].z = lh ? 1.0f : -1.0f;
	m->r[0].y = 1.0f / tx;
	m->r[1].x = -1.0f / ty;
	m->r[1].z = -m->r[3].z * shift / ty;
	m->r[1].w = iod / 2.0f;
	m->r[2].z = -m->r[3].z * n / (n - f);
	m->r[2].w = n * f / (n - f);
}

void Mtx_PerspTilt(C3D_Mtx* m, float fovx, float invaspect, float n, float f, bool lh)
{
	Mtx_PerspStereoTilt(m, fovx, invaspect, n, f, 0.0f, 0.0f, lh);
}

void Mtx_LookAt(C3D_Mtx* out, C3D_FVec eye, C3D_FVec target, C3D_FVec up, bool lh)
{
	C3D_FVec z = FVec3_Normalize(lh ? FVec3_Subtract(target, eye) : FVec3_Subtract(eye, target));
	C3D_FVec x = FVec3_Normalize(FVec3_Cross(up, z));
	C3D_FVec y = FVec3_Cross(z, x);
	out->r[0] = FVec4_New(x.x, x.y, x.z, -FVec3_Dot(x, eye));
	out->r[1] = FVec4_New(y.x, y.y, y.z, -FVec3_Dot(y, eye));
	out->r[2] = FVec4_New(z.x, z.y, z.z, -FVec3_Dot(z, eye));
	out->r[3] = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
}
