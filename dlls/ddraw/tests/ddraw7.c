/*
 * Copyright 2006, 2012-2013 Stefan Dösinger for CodeWeavers
 * Copyright 2011 Henri Verbeet for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#define COBJMACROS

#include "wine/test.h"
#include <limits.h>
#include "d3d.h"

static HRESULT (WINAPI *pDirectDrawCreateEx)(GUID *guid, void **ddraw, REFIID iid, IUnknown *outer_unknown);

struct vec2
{
    float x, y;
};

struct vec3
{
    float x, y, z;
};

struct vec4
{
    float x, y, z, w;
};

struct create_window_thread_param
{
    HWND window;
    HANDLE window_created;
    HANDLE destroy_window;
    HANDLE thread;
};

static BOOL compare_float(float f, float g, unsigned int ulps)
{
    int x = *(int *)&f;
    int y = *(int *)&g;

    if (x < 0)
        x = INT_MIN - x;
    if (y < 0)
        y = INT_MIN - y;

    if (abs(x - y) > ulps)
        return FALSE;

    return TRUE;
}

static BOOL compare_vec3(struct vec3 *vec, float x, float y, float z, unsigned int ulps)
{
    return compare_float(vec->x, x, ulps)
            && compare_float(vec->y, y, ulps)
            && compare_float(vec->z, z, ulps);
}

static BOOL compare_vec4(struct vec4 *vec, float x, float y, float z, float w, unsigned int ulps)
{
    return compare_float(vec->x, x, ulps)
            && compare_float(vec->y, y, ulps)
            && compare_float(vec->z, z, ulps)
            && compare_float(vec->w, w, ulps);
}

static BOOL compare_color(D3DCOLOR c1, D3DCOLOR c2, BYTE max_diff)
{
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff) return FALSE;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff) return FALSE;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff) return FALSE;
    c1 >>= 8; c2 >>= 8;
    if (abs((c1 & 0xff) - (c2 & 0xff)) > max_diff) return FALSE;
    return TRUE;
}

static DWORD WINAPI create_window_thread_proc(void *param)
{
    struct create_window_thread_param *p = param;
    DWORD res;
    BOOL ret;

    p->window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    ret = SetEvent(p->window_created);
    ok(ret, "SetEvent failed, last error %#x.\n", GetLastError());

    for (;;)
    {
        MSG msg;

        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
            DispatchMessage(&msg);
        res = WaitForSingleObject(p->destroy_window, 100);
        if (res == WAIT_OBJECT_0)
            break;
        if (res != WAIT_TIMEOUT)
        {
            ok(0, "Wait failed (%#x), last error %#x.\n", res, GetLastError());
            break;
        }
    }

    DestroyWindow(p->window);

    return 0;
}

static void create_window_thread(struct create_window_thread_param *p)
{
    DWORD res, tid;

    p->window_created = CreateEvent(NULL, FALSE, FALSE, NULL);
    ok(!!p->window_created, "CreateEvent failed, last error %#x.\n", GetLastError());
    p->destroy_window = CreateEvent(NULL, FALSE, FALSE, NULL);
    ok(!!p->destroy_window, "CreateEvent failed, last error %#x.\n", GetLastError());
    p->thread = CreateThread(NULL, 0, create_window_thread_proc, p, 0, &tid);
    ok(!!p->thread, "Failed to create thread, last error %#x.\n", GetLastError());
    res = WaitForSingleObject(p->window_created, INFINITE);
    ok(res == WAIT_OBJECT_0, "Wait failed (%#x), last error %#x.\n", res, GetLastError());
}

static void destroy_window_thread(struct create_window_thread_param *p)
{
    SetEvent(p->destroy_window);
    WaitForSingleObject(p->thread, INFINITE);
    CloseHandle(p->destroy_window);
    CloseHandle(p->window_created);
    CloseHandle(p->thread);
}

static IDirectDrawSurface7 *get_depth_stencil(IDirect3DDevice7 *device)
{
    IDirectDrawSurface7 *rt, *ret;
    DDSCAPS2 caps = {DDSCAPS_ZBUFFER, 0, 0, 0};
    HRESULT hr;

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get the render target, hr %#x.\n", hr);
    hr = IDirectDrawSurface7_GetAttachedSurface(rt, &caps, &ret);
    ok(SUCCEEDED(hr) || hr == DDERR_NOTFOUND, "Failed to get the z buffer, hr %#x.\n", hr);
    IDirectDrawSurface7_Release(rt);
    return ret;
}

static D3DCOLOR get_surface_color(IDirectDrawSurface7 *surface, UINT x, UINT y)
{
    RECT rect = {x, y, x + 1, y + 1};
    DDSURFACEDESC2 surface_desc;
    D3DCOLOR color;
    HRESULT hr;

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);

    hr = IDirectDrawSurface7_Lock(surface, &rect, &surface_desc, DDLOCK_READONLY, NULL);
    ok(SUCCEEDED(hr), "Failed to lock surface, hr %#x.\n", hr);
    if (FAILED(hr))
        return 0xdeadbeef;

    color = *((DWORD *)surface_desc.lpSurface) & 0x00ffffff;

    hr = IDirectDrawSurface7_Unlock(surface, &rect);
    ok(SUCCEEDED(hr), "Failed to unlock surface, hr %#x.\n", hr);

    return color;
}

static HRESULT CALLBACK enum_z_fmt(DDPIXELFORMAT *format, void *ctx)
{
    DDPIXELFORMAT *z_fmt = ctx;

    if (U1(*format).dwZBufferBitDepth > U1(*z_fmt).dwZBufferBitDepth)
        *z_fmt = *format;

    return DDENUMRET_OK;
}

static IDirectDraw7 *create_ddraw(void)
{
    IDirectDraw7 *ddraw;

    if (FAILED(pDirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL)))
        return NULL;

    return ddraw;
}

static HRESULT WINAPI enum_devtype_cb(char *desc_str, char *name, D3DDEVICEDESC7 *desc, void *ctx)
{
    BOOL *hal_ok = ctx;
    if (IsEqualGUID(&desc->deviceGUID, &IID_IDirect3DTnLHalDevice))
    {
        *hal_ok = TRUE;
        return DDENUMRET_CANCEL;
    }
    return DDENUMRET_OK;
}

static IDirect3DDevice7 *create_device(HWND window, DWORD coop_level)
{
    IDirectDrawSurface7 *surface, *ds;
    IDirect3DDevice7 *device = NULL;
    DDSURFACEDESC2 surface_desc;
    DDPIXELFORMAT z_fmt;
    IDirectDraw7 *ddraw;
    IDirect3D7 *d3d7;
    HRESULT hr;
    BOOL hal_ok = FALSE;
    const GUID *devtype = &IID_IDirect3DHALDevice;

    if (!(ddraw = create_ddraw()))
        return NULL;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, coop_level);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    surface_desc.dwWidth = 640;
    surface_desc.dwHeight = 480;

    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n", hr);

    if (coop_level & DDSCL_NORMAL)
    {
        IDirectDrawClipper *clipper;

        hr = IDirectDraw7_CreateClipper(ddraw, 0, &clipper, NULL);
        ok(SUCCEEDED(hr), "Failed to create clipper, hr %#x.\n", hr);
        hr = IDirectDrawClipper_SetHWnd(clipper, 0, window);
        ok(SUCCEEDED(hr), "Failed to set clipper window, hr %#x.\n", hr);
        hr = IDirectDrawSurface7_SetClipper(surface, clipper);
        ok(SUCCEEDED(hr), "Failed to set surface clipper, hr %#x.\n", hr);
        IDirectDrawClipper_Release(clipper);
    }

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d7);
    IDirectDraw7_Release(ddraw);
    if (FAILED(hr))
    {
        IDirectDrawSurface7_Release(surface);
        return NULL;
    }

    hr = IDirect3D7_EnumDevices(d3d7, enum_devtype_cb, &hal_ok);
    ok(SUCCEEDED(hr), "Failed to enumerate devices, hr %#x.\n", hr);
    if (hal_ok) devtype = &IID_IDirect3DTnLHalDevice;

    memset(&z_fmt, 0, sizeof(z_fmt));
    hr = IDirect3D7_EnumZBufferFormats(d3d7, devtype, enum_z_fmt, &z_fmt);
    if (FAILED(hr) || !z_fmt.dwSize)
    {
        IDirect3D7_Release(d3d7);
        IDirectDrawSurface7_Release(surface);
        return NULL;
    }

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER;
    U4(surface_desc).ddpfPixelFormat = z_fmt;
    surface_desc.dwWidth = 640;
    surface_desc.dwHeight = 480;
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &ds, NULL);
    ok(SUCCEEDED(hr), "Failed to create depth buffer, hr %#x.\n", hr);
    if (FAILED(hr))
    {
        IDirect3D7_Release(d3d7);
        IDirectDrawSurface7_Release(surface);
        return NULL;
    }

    hr = IDirectDrawSurface_AddAttachedSurface(surface, ds);
    ok(SUCCEEDED(hr), "Failed to attach depth buffer, hr %#x.\n", hr);
    IDirectDrawSurface7_Release(ds);
    if (FAILED(hr))
    {
        IDirect3D7_Release(d3d7);
        IDirectDrawSurface7_Release(surface);
        return NULL;
    }

    hr = IDirect3D7_CreateDevice(d3d7, devtype, surface, &device);
    IDirect3D7_Release(d3d7);
    IDirectDrawSurface7_Release(surface);
    if (FAILED(hr))
        return NULL;

    return device;
}

static const UINT *expect_messages;

static LRESULT CALLBACK test_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (expect_messages && message == *expect_messages)
        ++expect_messages;

    return DefWindowProcA(hwnd, message, wparam, lparam);
}

/* Set the wndproc back to what ddraw expects it to be, and release the ddraw
 * interface. This prevents subsequent SetCooperativeLevel() calls on a
 * different window from failing with DDERR_HWNDALREADYSET. */
static void fix_wndproc(HWND window, LONG_PTR proc)
{
    IDirectDraw7 *ddraw;
    HRESULT hr;

    if (!(ddraw = create_ddraw()))
        return;

    SetWindowLongPtrA(window, GWLP_WNDPROC, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    IDirectDraw7_Release(ddraw);
}

static void test_process_vertices(void)
{
    IDirect3DVertexBuffer7 *src_vb, *dst_vb1, *dst_vb2;
    D3DVERTEXBUFFERDESC vb_desc;
    IDirect3DDevice7 *device;
    struct vec4 *dst_data;
    struct vec3 *dst_data2;
    struct vec3 *src_data;
    IDirect3D7 *d3d7;
    D3DVIEWPORT7 vp;
    HWND window;
    HRESULT hr;

    static D3DMATRIX world =
    {
        0.0f,  1.0f, 0.0f, 0.0f,
        1.0f,  0.0f, 0.0f, 0.0f,
        0.0f,  0.0f, 0.0f, 1.0f,
        0.0f,  1.0f, 1.0f, 1.0f,
    };
    static D3DMATRIX view =
    {
        2.0f,  0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        0.0f,  0.0f, 1.0f, 0.0f,
        0.0f,  0.0f, 0.0f, 3.0f,
    };
    static D3DMATRIX proj =
    {
        1.0f,  0.0f, 0.0f, 1.0f,
        0.0f,  1.0f, 1.0f, 0.0f,
        0.0f,  1.0f, 1.0f, 0.0f,
        1.0f,  0.0f, 0.0f, 1.0f,
    };

    window = CreateWindowA("static", "d3d7_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d7);
    ok(SUCCEEDED(hr), "Failed to get Direct3D7 interface, hr %#x.\n", hr);

    memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.dwSize = sizeof(vb_desc);
    vb_desc.dwFVF = D3DFVF_XYZ;
    vb_desc.dwNumVertices = 4;
    hr = IDirect3D7_CreateVertexBuffer(d3d7, &vb_desc, &src_vb, 0);
    ok(SUCCEEDED(hr), "Failed to create source vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(src_vb, 0, (void **)&src_data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock source vertex buffer, hr %#x.\n", hr);
    src_data[0].x = 0.0f;
    src_data[0].y = 0.0f;
    src_data[0].z = 0.0f;
    src_data[1].x = 1.0f;
    src_data[1].y = 1.0f;
    src_data[1].z = 1.0f;
    src_data[2].x = -1.0f;
    src_data[2].y = -1.0f;
    src_data[2].z = 0.5f;
    src_data[3].x = 0.5f;
    src_data[3].y = -0.5f;
    src_data[3].z = 0.25f;
    hr = IDirect3DVertexBuffer7_Unlock(src_vb);
    ok(SUCCEEDED(hr), "Failed to unlock source vertex buffer, hr %#x.\n", hr);

    memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.dwSize = sizeof(vb_desc);
    vb_desc.dwFVF = D3DFVF_XYZRHW;
    vb_desc.dwNumVertices = 4;
    /* MSDN says that the last parameter must be 0 - check that. */
    hr = IDirect3D7_CreateVertexBuffer(d3d7, &vb_desc, &dst_vb1, 4);
    ok(SUCCEEDED(hr), "Failed to create vertex buffer, hr %#x.\n", hr);

    memset(&vb_desc, 0, sizeof(vb_desc));
    vb_desc.dwSize = sizeof(vb_desc);
    vb_desc.dwFVF = D3DFVF_XYZ;
    vb_desc.dwNumVertices = 5;
    /* MSDN says that the last parameter must be 0 - check that. */
    hr = IDirect3D7_CreateVertexBuffer(d3d7, &vb_desc, &dst_vb2, 12345678);
    ok(SUCCEEDED(hr), "Failed to create vertex buffer, hr %#x.\n", hr);

    memset(&vp, 0, sizeof(vp));
    vp.dwX = 64;
    vp.dwY = 64;
    vp.dwWidth = 128;
    vp.dwHeight = 128;
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;
    hr = IDirect3DDevice7_SetViewport(device, &vp);
    ok(SUCCEEDED(hr), "Failed to set viewport, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_ProcessVertices(dst_vb1, D3DVOP_TRANSFORM, 0, 4, src_vb, 0, device, 0);
    ok(SUCCEEDED(hr), "Failed to process vertices, hr %#x.\n", hr);
    hr = IDirect3DVertexBuffer7_ProcessVertices(dst_vb2, D3DVOP_TRANSFORM, 0, 4, src_vb, 0, device, 0);
    ok(SUCCEEDED(hr), "Failed to process vertices, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(dst_vb1, 0, (void **)&dst_data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock destination vertex buffer, hr %#x.\n", hr);
    ok(compare_vec4(&dst_data[0], +1.280e+2f, +1.280e+2f, +0.000e+0f, +1.000e+0f, 4096),
            "Got unexpected vertex 0 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[0].x, dst_data[0].y, dst_data[0].z, dst_data[0].w);
    ok(compare_vec4(&dst_data[1], +1.920e+2f, +6.400e+1f, +1.000e+0f, +1.000e+0f, 4096),
            "Got unexpected vertex 1 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[1].x, dst_data[1].y, dst_data[1].z, dst_data[1].w);
    ok(compare_vec4(&dst_data[2], +6.400e+1f, +1.920e+2f, +5.000e-1f, +1.000e+0f, 4096),
            "Got unexpected vertex 2 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[2].x, dst_data[2].y, dst_data[2].z, dst_data[2].w);
    ok(compare_vec4(&dst_data[3], +1.600e+2f, +1.600e+2f, +2.500e-1f, +1.000e+0f, 4096),
            "Got unexpected vertex 3 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[3].x, dst_data[3].y, dst_data[3].z, dst_data[3].w);
    hr = IDirect3DVertexBuffer7_Unlock(dst_vb1);
    ok(SUCCEEDED(hr), "Failed to unlock destination vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(dst_vb2, 0, (void **)&dst_data2, NULL);
    ok(SUCCEEDED(hr), "Failed to lock destination vertex buffer, hr %#x.\n", hr);
    /* Small thing without much practical meaning, but I stumbled upon it,
     * so let's check for it: If the output vertex buffer has no RHW value,
     * the RHW value of the last vertex is written into the next vertex. */
    ok(compare_vec3(&dst_data2[4], +1.000e+0f, +0.000e+0f, +0.000e+0f, 4096),
            "Got unexpected vertex 4 {%.8e, %.8e, %.8e}.\n",
            dst_data2[4].x, dst_data2[4].y, dst_data2[4].z);
    hr = IDirect3DVertexBuffer7_Unlock(dst_vb2);
    ok(SUCCEEDED(hr), "Failed to unlock destination vertex buffer, hr %#x.\n", hr);

    /* Try a more complicated viewport, same vertices. */
    memset(&vp, 0, sizeof(vp));
    vp.dwX = 10;
    vp.dwY = 5;
    vp.dwWidth = 246;
    vp.dwHeight = 130;
    vp.dvMinZ = -2.0f;
    vp.dvMaxZ = 4.0f;
    hr = IDirect3DDevice7_SetViewport(device, &vp);
    ok(SUCCEEDED(hr), "Failed to set viewport, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_ProcessVertices(dst_vb1, D3DVOP_TRANSFORM, 0, 4, src_vb, 0, device, 0);
    ok(SUCCEEDED(hr), "Failed to process vertices, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(dst_vb1, 0, (void **)&dst_data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock destination vertex buffer, hr %#x.\n", hr);
    ok(compare_vec4(&dst_data[0], +1.330e+2f, +7.000e+1f, -2.000e+0f, +1.000e+0f, 4096),
            "Got unexpected vertex 0 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[0].x, dst_data[0].y, dst_data[0].z, dst_data[0].w);
    ok(compare_vec4(&dst_data[1], +2.560e+2f, +5.000e+0f, +4.000e+0f, +1.000e+0f, 4096),
            "Got unexpected vertex 1 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[1].x, dst_data[1].y, dst_data[1].z, dst_data[1].w);
    ok(compare_vec4(&dst_data[2], +1.000e+1f, +1.350e+2f, +1.000e+0f, +1.000e+0f, 4096),
            "Got unexpected vertex 2 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[2].x, dst_data[2].y, dst_data[2].z, dst_data[2].w);
    ok(compare_vec4(&dst_data[3], +1.945e+2f, +1.025e+2f, -5.000e-1f, +1.000e+0f, 4096),
            "Got unexpected vertex 3 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[3].x, dst_data[3].y, dst_data[3].z, dst_data[3].w);
    hr = IDirect3DVertexBuffer7_Unlock(dst_vb1);
    ok(SUCCEEDED(hr), "Failed to unlock destination vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &world);
    ok(SUCCEEDED(hr), "Failed to set world transform, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_VIEW, &view);
    ok(SUCCEEDED(hr), "Failed to set view transform, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_PROJECTION, &proj);
    ok(SUCCEEDED(hr), "Failed to set projection transform, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_ProcessVertices(dst_vb1, D3DVOP_TRANSFORM, 0, 4, src_vb, 0, device, 0);
    ok(SUCCEEDED(hr), "Failed to process vertices, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(dst_vb1, 0, (void **)&dst_data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock destination vertex buffer, hr %#x.\n", hr);
    ok(compare_vec4(&dst_data[0], +2.560e+2f, +7.000e+1f, -2.000e+0f, +3.333e-1f, 4096),
            "Got unexpected vertex 0 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[0].x, dst_data[0].y, dst_data[0].z, dst_data[0].w);
    ok(compare_vec4(&dst_data[1], +2.560e+2f, +7.813e+1f, -2.750e+0f, +1.250e-1f, 4096),
            "Got unexpected vertex 1 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[1].x, dst_data[1].y, dst_data[1].z, dst_data[1].w);
    ok(compare_vec4(&dst_data[2], +2.560e+2f, +4.400e+1f, +4.000e-1f, +4.000e-1f, 4096),
            "Got unexpected vertex 2 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[2].x, dst_data[2].y, dst_data[2].z, dst_data[2].w);
    ok(compare_vec4(&dst_data[3], +2.560e+2f, +8.182e+1f, -3.091e+0f, +3.636e-1f, 4096),
            "Got unexpected vertex 3 {%.8e, %.8e, %.8e, %.8e}.\n",
            dst_data[3].x, dst_data[3].y, dst_data[3].z, dst_data[3].w);
    hr = IDirect3DVertexBuffer7_Unlock(dst_vb1);
    ok(SUCCEEDED(hr), "Failed to unlock destination vertex buffer, hr %#x.\n", hr);

    IDirect3DVertexBuffer7_Release(dst_vb2);
    IDirect3DVertexBuffer7_Release(dst_vb1);
    IDirect3DVertexBuffer7_Release(src_vb);
    IDirect3D7_Release(d3d7);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_coop_level_create_device_window(void)
{
    HWND focus_window, device_window;
    IDirectDraw7 *ddraw;
    HRESULT hr;

    focus_window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a 3D device, skipping test.\n");
        DestroyWindow(focus_window);
        return;
    }

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW);
    ok(hr == DDERR_INVALIDPARAMS, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW | DDSCL_NORMAL);
    ok(hr == DDERR_INVALIDPARAMS, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW | DDSCL_NORMAL | DDSCL_FULLSCREEN);
    ok(hr == DDERR_INVALIDPARAMS, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DDERR_NOFOCUSWINDOW || broken(hr == DDERR_INVALIDPARAMS), "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");

    /* Windows versions before 98 / NT5 don't support DDSCL_CREATEDEVICEWINDOW. */
    if (broken(hr == DDERR_INVALIDPARAMS))
    {
        win_skip("DDSCL_CREATEDEVICEWINDOW not supported, skipping test.\n");
        IDirectDraw7_Release(ddraw);
        DestroyWindow(focus_window);
        return;
    }

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, focus_window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_SETFOCUSWINDOW
            | DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DDERR_NOHWND, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!!device_window, "Device window not found.\n");

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, focus_window, DDSCL_SETFOCUSWINDOW
            | DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!!device_window, "Device window not found.\n");

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DDERR_NOFOCUSWINDOW, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, focus_window, DDSCL_SETFOCUSWINDOW);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!device_window, "Unexpected device window found.\n");
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, NULL, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(hr == DD_OK, "Got unexpected hr %#x.\n", hr);
    device_window = FindWindowA("DirectDrawDeviceWnd", "DirectDrawDeviceWnd");
    ok(!!device_window, "Device window not found.\n");

    IDirectDraw7_Release(ddraw);
    DestroyWindow(focus_window);
}

static void test_clipper_blt(void)
{
    IDirectDrawSurface7 *src_surface, *dst_surface;
    RECT client_rect, src_rect;
    IDirectDrawClipper *clipper;
    DDSURFACEDESC2 surface_desc;
    unsigned int i, j, x, y;
    IDirectDraw7 *ddraw;
    RGNDATA *rgn_data;
    D3DCOLOR color;
    HRGN r1, r2;
    HWND window;
    DDBLTFX fx;
    HRESULT hr;
    DWORD *ptr;
    DWORD ret;

    static const DWORD src_data[] =
    {
        0xff0000ff, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xffffffff,
        0xff0000ff, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xffffffff,
        0xff0000ff, 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff, 0xffffffff,
    };
    static const D3DCOLOR expected1[] =
    {
        0x000000ff, 0x0000ff00, 0x00000000, 0x00000000,
        0x000000ff, 0x0000ff00, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00ff0000, 0x00ffffff,
        0x00000000, 0x00000000, 0x00ff0000, 0x00ffffff,
    };
    static const D3DCOLOR expected2[] =
    {
        0x000000ff, 0x000000ff, 0x00000000, 0x00000000,
        0x000000ff, 0x000000ff, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x000000ff, 0x000000ff,
        0x00000000, 0x00000000, 0x000000ff, 0x000000ff,
    };

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            10, 10, 640, 480, 0, 0, 0, 0);
    ShowWindow(window, SW_SHOW);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    ret = GetClientRect(window, &client_rect);
    ok(ret, "Failed to get client rect.\n");
    ret = MapWindowPoints(window, NULL, (POINT *)&client_rect, 2);
    ok(ret, "Failed to map client rect.\n");

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);

    hr = IDirectDraw7_CreateClipper(ddraw, 0, &clipper, NULL);
    ok(SUCCEEDED(hr), "Failed to create clipper, hr %#x.\n", hr);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(hr == DDERR_NOCLIPLIST, "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawClipper_SetHWnd(clipper, 0, window);
    ok(SUCCEEDED(hr), "Failed to set clipper window, hr %#x.\n", hr);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(SUCCEEDED(hr), "Failed to get clip list size, hr %#x.\n", hr);
    rgn_data = HeapAlloc(GetProcessHeap(), 0, ret);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, rgn_data, &ret);
    ok(SUCCEEDED(hr), "Failed to get clip list, hr %#x.\n", hr);
    ok(rgn_data->rdh.dwSize == sizeof(rgn_data->rdh), "Got unexpected structure size %#x.\n", rgn_data->rdh.dwSize);
    ok(rgn_data->rdh.iType == RDH_RECTANGLES, "Got unexpected type %#x.\n", rgn_data->rdh.iType);
    ok(rgn_data->rdh.nCount >= 1, "Got unexpected count %u.\n", rgn_data->rdh.nCount);
    ok(EqualRect(&rgn_data->rdh.rcBound, &client_rect),
            "Got unexpected bounding rect {%d, %d, %d, %d}, expected {%d, %d, %d, %d}.\n",
            rgn_data->rdh.rcBound.left, rgn_data->rdh.rcBound.top,
            rgn_data->rdh.rcBound.right, rgn_data->rdh.rcBound.bottom,
            client_rect.left, client_rect.top, client_rect.right, client_rect.bottom);
    HeapFree(GetProcessHeap(), 0, rgn_data);

    r1 = CreateRectRgn(0, 0, 320, 240);
    ok(!!r1, "Failed to create region.\n");
    r2 = CreateRectRgn(320, 240, 640, 480);
    ok(!!r2, "Failed to create region.\n");
    CombineRgn(r1, r1, r2, RGN_OR);
    ret = GetRegionData(r1, 0, NULL);
    rgn_data = HeapAlloc(GetProcessHeap(), 0, ret);
    ret = GetRegionData(r1, ret, rgn_data);
    ok(!!ret, "Failed to get region data.\n");

    DeleteObject(r2);
    DeleteObject(r1);

    hr = IDirectDrawClipper_SetClipList(clipper, rgn_data, 0);
    ok(hr == DDERR_CLIPPERISUSINGHWND, "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawClipper_SetHWnd(clipper, 0, NULL);
    ok(SUCCEEDED(hr), "Failed to set clipper window, hr %#x.\n", hr);
    hr = IDirectDrawClipper_SetClipList(clipper, rgn_data, 0);
    ok(SUCCEEDED(hr), "Failed to set clip list, hr %#x.\n", hr);

    HeapFree(GetProcessHeap(), 0, rgn_data);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    surface_desc.dwWidth = 640;
    surface_desc.dwHeight = 480;
    U4(surface_desc).ddpfPixelFormat.dwSize = sizeof(U4(surface_desc).ddpfPixelFormat);
    U4(surface_desc).ddpfPixelFormat.dwFlags = DDPF_RGB;
    U1(U4(surface_desc).ddpfPixelFormat).dwRGBBitCount = 32;
    U2(U4(surface_desc).ddpfPixelFormat).dwRBitMask = 0x00ff0000;
    U3(U4(surface_desc).ddpfPixelFormat).dwGBitMask = 0x0000ff00;
    U4(U4(surface_desc).ddpfPixelFormat).dwBBitMask = 0x000000ff;

    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &src_surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create source surface, hr %#x.\n", hr);
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &dst_surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create destination surface, hr %#x.\n", hr);

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    hr = IDirectDrawSurface7_Blt(src_surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(SUCCEEDED(hr), "Failed to clear source surface, hr %#x.\n", hr);
    hr = IDirectDrawSurface7_Blt(dst_surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(SUCCEEDED(hr), "Failed to clear destination surface, hr %#x.\n", hr);

    hr = IDirectDrawSurface7_Lock(src_surface, NULL, &surface_desc, 0, NULL);
    ok(SUCCEEDED(hr), "Failed to lock source surface, hr %#x.\n", hr);
    ok(U1(surface_desc).lPitch == 2560, "Got unexpected surface pitch %u.\n", U1(surface_desc).lPitch);
    ptr = surface_desc.lpSurface;
    memcpy(&ptr[   0], &src_data[ 0], 6 * sizeof(DWORD));
    memcpy(&ptr[ 640], &src_data[ 6], 6 * sizeof(DWORD));
    memcpy(&ptr[1280], &src_data[12], 6 * sizeof(DWORD));
    hr = IDirectDrawSurface7_Unlock(src_surface, NULL);
    ok(SUCCEEDED(hr), "Failed to unlock source surface, hr %#x.\n", hr);

    hr = IDirectDrawSurface7_SetClipper(dst_surface, clipper);
    ok(SUCCEEDED(hr), "Failed to set clipper, hr %#x.\n", hr);

    SetRect(&src_rect, 1, 1, 5, 2);
    hr = IDirectDrawSurface7_Blt(dst_surface, NULL, src_surface, &src_rect, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Failed to blit, hr %#x.\n", hr);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            x = 80 * ((2 * j) + 1);
            y = 60 * ((2 * i) + 1);
            color = get_surface_color(dst_surface, x, y);
            ok(compare_color(color, expected1[i * 4 + j], 1),
                    "Expected color 0x%08x at %u,%u, got 0x%08x.\n", expected1[i * 4 + j], x, y, color);
        }
    }

    U5(fx).dwFillColor = 0xff0000ff;
    hr = IDirectDrawSurface7_Blt(dst_surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(SUCCEEDED(hr), "Failed to clear destination surface, hr %#x.\n", hr);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            x = 80 * ((2 * j) + 1);
            y = 60 * ((2 * i) + 1);
            color = get_surface_color(dst_surface, x, y);
            ok(compare_color(color, expected2[i * 4 + j], 1),
                    "Expected color 0x%08x at %u,%u, got 0x%08x.\n", expected2[i * 4 + j], x, y, color);
        }
    }

    hr = IDirectDrawSurface7_BltFast(dst_surface, 0, 0, src_surface, NULL, DDBLTFAST_WAIT);
    ok(hr == DDERR_BLTFASTCANTCLIP, "Got unexpected hr %#x.\n", hr);

    hr = IDirectDrawClipper_SetHWnd(clipper, 0, window);
    ok(SUCCEEDED(hr), "Failed to set clipper window, hr %#x.\n", hr);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(SUCCEEDED(hr), "Failed to get clip list size, hr %#x.\n", hr);
    DestroyWindow(window);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(hr == E_FAIL, "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawClipper_SetHWnd(clipper, 0, NULL);
    ok(SUCCEEDED(hr), "Failed to set clipper window, hr %#x.\n", hr);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(SUCCEEDED(hr), "Failed to get clip list size, hr %#x.\n", hr);
    hr = IDirectDrawClipper_SetClipList(clipper, NULL, 0);
    ok(SUCCEEDED(hr), "Failed to set clip list, hr %#x.\n", hr);
    hr = IDirectDrawClipper_GetClipList(clipper, NULL, NULL, &ret);
    ok(hr == DDERR_NOCLIPLIST, "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawSurface7_Blt(dst_surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(hr == DDERR_NOCLIPLIST, "Got unexpected hr %#x.\n", hr);

    IDirectDrawSurface7_Release(dst_surface);
    IDirectDrawSurface7_Release(src_surface);
    IDirectDrawClipper_Release(clipper);
    IDirectDraw7_Release(ddraw);
}

static void test_coop_level_d3d_state(void)
{
    IDirectDrawSurface7 *rt, *surface;
    IDirect3DDevice7 *device;
    IDirectDraw7 *ddraw;
    IDirect3D7 *d3d;
    D3DCOLOR color;
    DWORD value;
    HWND window;
    HRESULT hr;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ZENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!!value, "Got unexpected z-enable state %#x.\n", value);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!value, "Got unexpected alpha blend enable state %#x.\n", value);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
    ok(SUCCEEDED(hr), "Failed to set render state, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 0.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x00ff0000, 1), "Got unexpected color 0x%08x.\n", color);

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get d3d interface, hr %#x.\n", hr);
    hr = IDirect3D7_QueryInterface(d3d, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to get ddraw interface, hr %#x.\n", hr);
    IDirect3D7_Release(d3d);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);
    hr = IDirectDrawSurface7_IsLost(rt);
    ok(hr == DDERR_SURFACELOST, "Got unexpected hr %#x.\n", hr);
    hr = IDirectDraw7_RestoreAllSurfaces(ddraw);
    ok(SUCCEEDED(hr), "Failed to restore surfaces, hr %#x.\n", hr);
    IDirectDraw7_Release(ddraw);

    hr = IDirect3DDevice7_GetRenderTarget(device, &surface);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);
    ok(surface == rt, "Got unexpected surface %p.\n", surface);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ZENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!!value, "Got unexpected z-enable state %#x.\n", value);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!!value, "Got unexpected alpha blend enable state %#x.\n", value);
    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 0.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x0000ff00, 1), "Got unexpected color 0x%08x.\n", color);

    IDirectDrawSurface7_Release(surface);
    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_surface_interface_mismatch(void)
{
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    IDirectDrawSurface7 *surface = NULL, *ds;
    IDirectDrawSurface3 *surface3 = NULL;
    IDirect3DDevice7 *device = NULL;
    DDSURFACEDESC2 surface_desc;
    DDPIXELFORMAT z_fmt;
    ULONG refcount;
    HRESULT hr;
    D3DCOLOR color;
    HWND window;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        goto cleanup;
    }

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    surface_desc.dwWidth = 640;
    surface_desc.dwHeight = 480;

    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n", hr);

    hr = IDirectDrawSurface7_QueryInterface(surface, &IID_IDirectDrawSurface3, (void **)&surface3);
    ok(SUCCEEDED(hr), "Failed to QI IDirectDrawSurface3, hr %#x.\n", hr);

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr))
    {
        skip("Failed to get the IDirect3D7 interface, skipping test.\n");
        goto cleanup;
    }

    memset(&z_fmt, 0, sizeof(z_fmt));
    hr = IDirect3D7_EnumZBufferFormats(d3d, &IID_IDirect3DHALDevice, enum_z_fmt, &z_fmt);
    if (FAILED(hr) || !z_fmt.dwSize)
    {
        skip("No depth buffer formats available, skipping test.\n");
        goto cleanup;
    }

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER;
    U4(surface_desc).ddpfPixelFormat = z_fmt;
    surface_desc.dwWidth = 640;
    surface_desc.dwHeight = 480;
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &ds, NULL);
    ok(SUCCEEDED(hr), "Failed to create depth buffer, hr %#x.\n", hr);
    if (FAILED(hr))
        goto cleanup;

    /* Using a different surface interface version still works */
    hr = IDirectDrawSurface3_AddAttachedSurface(surface3, (IDirectDrawSurface3 *)ds);
    ok(SUCCEEDED(hr), "Failed to attach depth buffer, hr %#x.\n", hr);
    refcount = IDirectDrawSurface7_Release(ds);
    ok(refcount == 1, "Got unexpected refcount %u.\n", refcount);
    if (FAILED(hr))
        goto cleanup;

    /* Here too */
    hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, (IDirectDrawSurface7 *)surface3, &device);
    ok(SUCCEEDED(hr), "Failed to create d3d device.\n");
    if (FAILED(hr))
        goto cleanup;

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 0.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    color = get_surface_color(surface, 320, 240);
    ok(compare_color(color, 0x00ff0000, 1), "Got unexpected color 0x%08x.\n", color);

cleanup:
    if (surface3) IDirectDrawSurface3_Release(surface3);
    if (surface) IDirectDrawSurface7_Release(surface);
    if (device) IDirect3DDevice7_Release(device);
    if (d3d) IDirect3D7_Release(d3d);
    if (ddraw) IDirectDraw7_Release(ddraw);
    DestroyWindow(window);
}

static void test_coop_level_threaded(void)
{
    struct create_window_thread_param p;
    IDirectDraw7 *ddraw;
    HRESULT hr;

    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        return;
    }
    create_window_thread(&p);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, p.window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);

    IDirectDraw7_Release(ddraw);
    destroy_window_thread(&p);
}

static void test_depth_blit(void)
{
    IDirect3DDevice7 *device;
    static struct
    {
        float x, y, z;
        DWORD color;
    }
    quad1[] =
    {
        { -1.0,  1.0, 0.50f, 0xff00ff00},
        {  1.0,  1.0, 0.50f, 0xff00ff00},
        { -1.0, -1.0, 0.50f, 0xff00ff00},
        {  1.0, -1.0, 0.50f, 0xff00ff00},
    };
    static const D3DCOLOR expected_colors[4][4] =
    {
        {0x00ff0000, 0x00ff0000, 0x0000ff00, 0x0000ff00},
        {0x00ff0000, 0x00ff0000, 0x0000ff00, 0x0000ff00},
        {0x0000ff00, 0x0000ff00, 0x0000ff00, 0x0000ff00},
        {0x0000ff00, 0x0000ff00, 0x0000ff00, 0x0000ff00},
    };
    DDSURFACEDESC2 ddsd_new, ddsd_existing;

    IDirectDrawSurface7 *ds1, *ds2, *ds3, *rt;
    RECT src_rect, dst_rect;
    unsigned int i, j;
    D3DCOLOR color;
    HRESULT hr;
    IDirect3D7 *d3d;
    IDirectDraw7 *ddraw;
    DDBLTFX fx;
    HWND window;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get Direct3D7 interface, hr %#x.\n", hr);
    hr = IDirect3D7_QueryInterface(d3d, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to get DirectDraw7 interface, hr %#x.\n", hr);
    IDirect3D7_Release(d3d);

    ds1 = get_depth_stencil(device);

    memset(&ddsd_new, 0, sizeof(ddsd_new));
    ddsd_new.dwSize = sizeof(ddsd_new);
    memset(&ddsd_existing, 0, sizeof(ddsd_existing));
    ddsd_existing.dwSize = sizeof(ddsd_existing);
    hr = IDirectDrawSurface7_GetSurfaceDesc(ds1, &ddsd_existing);
    ddsd_new.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    ddsd_new.ddsCaps.dwCaps = DDSCAPS_ZBUFFER;
    ddsd_new.dwWidth = ddsd_existing.dwWidth;
    ddsd_new.dwHeight = ddsd_existing.dwHeight;
    U4(ddsd_new).ddpfPixelFormat = U4(ddsd_existing).ddpfPixelFormat;
    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd_new, &ds2, NULL);
    ok(SUCCEEDED(hr), "Failed to create a z buffer, hr %#x.\n", hr);
    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd_new, &ds3, NULL);
    ok(SUCCEEDED(hr), "Failed to create a z buffer, hr %#x.\n", hr);
    IDirectDraw7_Release(ddraw);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    ok(SUCCEEDED(hr), "Failed to enable z testing, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    ok(SUCCEEDED(hr), "Failed to set the z function, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    ok(SUCCEEDED(hr), "Failed to disable lighting, hr %#x.\n", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 0.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear the z buffer, hr %#x.\n", hr);

    /* Partial blit. */
    SetRect(&src_rect, 0, 0, 320, 240);
    SetRect(&dst_rect, 0, 0, 320, 240);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* Different locations. */
    SetRect(&src_rect, 0, 0, 320, 240);
    SetRect(&dst_rect, 320, 240, 640, 480);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* Streched. */
    SetRect(&src_rect, 0, 0, 320, 240);
    SetRect(&dst_rect, 0, 0, 640, 480);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* Flipped. */
    SetRect(&src_rect, 0, 480, 640, 0);
    SetRect(&dst_rect, 0, 0, 640, 480);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(hr == DDERR_INVALIDRECT, "Got unexpected hr %#x.\n", hr);
    SetRect(&src_rect, 0, 0, 640, 480);
    SetRect(&dst_rect, 0, 480, 640, 0);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(hr == DDERR_INVALIDRECT, "Got unexpected hr %#x.\n", hr);
    /* Full, explicit. */
    SetRect(&src_rect, 0, 0, 640, 480);
    SetRect(&dst_rect, 0, 0, 640, 480);
    hr = IDirectDrawSurface7_Blt(ds2, &dst_rect, ds1, &src_rect, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* Depth -> color blit: Succeeds on Win7 + Radeon HD 5700, fails on WinXP + Radeon X1600 */

    /* Depth blit inside a BeginScene / EndScene pair */
    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to start scene, hr %#x.\n", hr);
    /* From the current depth stencil */
    hr = IDirectDrawSurface7_Blt(ds2, NULL, ds1, NULL, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* To the current depth stencil */
    hr = IDirectDrawSurface7_Blt(ds1, NULL, ds2, NULL, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    /* Between unbound surfaces */
    hr = IDirectDrawSurface7_Blt(ds3, NULL, ds2, NULL, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

    /* Avoid changing the depth stencil, it doesn't work properly on Windows.
     * Instead use DDBLT_DEPTHFILL to clear the depth stencil. Unfortunately
     * drivers disagree on the meaning of dwFillDepth. Only 0 seems to produce
     * a reliable result(z = 0.0) */
    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    hr = IDirectDrawSurface7_Blt(ds2, NULL, NULL, NULL, DDBLT_DEPTHFILL | DDBLT_WAIT, &fx);
    ok(SUCCEEDED(hr), "Failed to clear the source z buffer, hr %#x.\n", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear the color and z buffers, hr %#x.\n", hr);
    SetRect(&dst_rect, 0, 0, 320, 240);
    hr = IDirectDrawSurface7_Blt(ds1, &dst_rect, ds2, NULL, DDBLT_WAIT, NULL);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    IDirectDrawSurface7_Release(ds3);
    IDirectDrawSurface7_Release(ds2);
    IDirectDrawSurface7_Release(ds1);

    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to start scene, hr %#x.\n", hr);
    hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZ | D3DFVF_DIFFUSE,
            quad1, 4, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            unsigned int x = 80 * ((2 * j) + 1);
            unsigned int y = 60 * ((2 * i) + 1);
            color = get_surface_color(rt, x, y);
            ok(compare_color(color, expected_colors[i][j], 1),
                    "Expected color 0x%08x at %u,%u, got 0x%08x.\n", expected_colors[i][j], x, y, color);
        }
    }

    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_texture_load_ckey(void)
{
    HWND window;
    IDirect3DDevice7 *device;
    IDirectDraw7 *ddraw;
    IDirectDrawSurface7 *src;
    IDirectDrawSurface7 *dst;
    DDSURFACEDESC2 ddsd;
    HRESULT hr;
    DDCOLORKEY ckey;
    IDirect3D7 *d3d;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get Direct3D7 interface, hr %#x.\n", hr);
    hr = IDirect3D7_QueryInterface(d3d, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to get DirectDraw7 interface, hr %#x.\n", hr);
    IDirect3D7_Release(d3d);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH;
    ddsd.dwHeight = 128;
    ddsd.dwWidth = 128;
    ddsd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &src, NULL);
    ok(SUCCEEDED(hr), "Failed to create source texture, hr %#x.\n", hr);
    ddsd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &dst, NULL);
    ok(SUCCEEDED(hr), "Failed to create destination texture, hr %#x.\n", hr);

    /* No surface has a color key */
    hr = IDirect3DDevice7_Load(device, dst, NULL, src, NULL, 0);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    ckey.dwColorSpaceLowValue = ckey.dwColorSpaceHighValue = 0xdeadbeef;
    hr = IDirectDrawSurface7_GetColorKey(dst, DDCKEY_SRCBLT, &ckey);
    ok(hr == DDERR_NOCOLORKEY, "Got unexpected hr %#x.\n", hr);
    ok(ckey.dwColorSpaceLowValue == 0xdeadbeef, "dwColorSpaceLowValue is %#x.\n", ckey.dwColorSpaceLowValue);
    ok(ckey.dwColorSpaceHighValue == 0xdeadbeef, "dwColorSpaceHighValue is %#x.\n", ckey.dwColorSpaceHighValue);

    /* Source surface has a color key */
    ckey.dwColorSpaceLowValue = ckey.dwColorSpaceHighValue = 0x0000ff00;
    hr = IDirectDrawSurface7_SetColorKey(src, DDCKEY_SRCBLT, &ckey);
    ok(SUCCEEDED(hr), "Failed to set color key, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Load(device, dst, NULL, src, NULL, 0);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawSurface7_GetColorKey(dst, DDCKEY_SRCBLT, &ckey);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    ok(ckey.dwColorSpaceLowValue == 0x0000ff00, "dwColorSpaceLowValue is %#x.\n", ckey.dwColorSpaceLowValue);
    ok(ckey.dwColorSpaceHighValue == 0x0000ff00, "dwColorSpaceHighValue is %#x.\n", ckey.dwColorSpaceHighValue);

    /* Both surfaces have a color key: Dest ckey is overwritten */
    ckey.dwColorSpaceLowValue = ckey.dwColorSpaceHighValue = 0x000000ff;
    hr = IDirectDrawSurface7_SetColorKey(dst, DDCKEY_SRCBLT, &ckey);
    ok(SUCCEEDED(hr), "Failed to set color key, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Load(device, dst, NULL, src, NULL, 0);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawSurface7_GetColorKey(dst, DDCKEY_SRCBLT, &ckey);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    ok(ckey.dwColorSpaceLowValue == 0x0000ff00, "dwColorSpaceLowValue is %#x.\n", ckey.dwColorSpaceLowValue);
    ok(ckey.dwColorSpaceHighValue == 0x0000ff00, "dwColorSpaceHighValue is %#x.\n", ckey.dwColorSpaceHighValue);

    /* Only the destination has a color key: It is deleted. This behavior differs from
     * IDirect3DTexture(2)::Load */
    hr = IDirectDrawSurface7_SetColorKey(src, DDCKEY_SRCBLT, NULL);
    ok(SUCCEEDED(hr), "Failed to set color key, hr %#x.\n", hr);
    hr = IDirectDrawSurface7_GetColorKey(src, DDCKEY_SRCBLT, &ckey);
    ok(hr == DDERR_NOCOLORKEY, "Got unexpected hr %#x.\n", hr);
    hr = IDirect3DDevice7_Load(device, dst, NULL, src, NULL, 0);
    ok(SUCCEEDED(hr), "Got unexpected hr %#x.\n", hr);
    hr = IDirectDrawSurface7_GetColorKey(dst, DDCKEY_SRCBLT, &ckey);
    todo_wine ok(hr == DDERR_NOCOLORKEY, "Got unexpected hr %#x.\n", hr);

    IDirectDrawSurface7_Release(dst);
    IDirectDrawSurface7_Release(src);
    IDirectDraw7_Release(ddraw);
    IDirect3DDevice7_Release(device);
}

static void test_zenable(void)
{
    static struct
    {
        struct vec4 position;
        D3DCOLOR diffuse;
    }
    tquad[] =
    {
        {{  0.0f, 480.0f, -0.5f, 1.0f}, 0xff00ff00},
        {{  0.0f,   0.0f, -0.5f, 1.0f}, 0xff00ff00},
        {{640.0f, 480.0f,  1.5f, 1.0f}, 0xff00ff00},
        {{640.0f,   0.0f,  1.5f, 1.0f}, 0xff00ff00},
    };
    IDirect3DDevice7 *device;
    IDirectDrawSurface7 *rt;
    D3DCOLOR color;
    HWND window;
    HRESULT hr;
    UINT x, y;
    UINT i, j;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    ok(SUCCEEDED(hr), "Failed to disable z-buffering, hr %#x.\n", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff0000, 0.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
    hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tquad, 4, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 4; ++j)
        {
            x = 80 * ((2 * j) + 1);
            y = 60 * ((2 * i) + 1);
            color = get_surface_color(rt, x, y);
            ok(compare_color(color, 0x0000ff00, 1),
                    "Expected color 0x0000ff00 at %u, %u, got 0x%08x.\n", x, y, color);
        }
    }
    IDirectDrawSurface7_Release(rt);

    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_ck_rgba(void)
{
    static struct
    {
        struct vec4 position;
        struct vec2 texcoord;
    }
    tquad[] =
    {
        {{  0.0f, 480.0f, 0.25f, 1.0f}, {0.0f, 0.0f}},
        {{  0.0f,   0.0f, 0.25f, 1.0f}, {0.0f, 1.0f}},
        {{640.0f, 480.0f, 0.25f, 1.0f}, {1.0f, 0.0f}},
        {{640.0f,   0.0f, 0.25f, 1.0f}, {1.0f, 1.0f}},
        {{  0.0f, 480.0f, 0.75f, 1.0f}, {0.0f, 0.0f}},
        {{  0.0f,   0.0f, 0.75f, 1.0f}, {0.0f, 1.0f}},
        {{640.0f, 480.0f, 0.75f, 1.0f}, {1.0f, 0.0f}},
        {{640.0f,   0.0f, 0.75f, 1.0f}, {1.0f, 1.0f}},
    };
    static const struct
    {
        D3DCOLOR fill_color;
        BOOL color_key;
        BOOL blend;
        D3DCOLOR result1;
        D3DCOLOR result2;
    }
    tests[] =
    {
        {0xff00ff00, TRUE,  TRUE,  0x00ff0000, 0x000000ff},
        {0xff00ff00, TRUE,  FALSE, 0x00ff0000, 0x000000ff},
        {0xff00ff00, FALSE, TRUE,  0x0000ff00, 0x0000ff00},
        {0xff00ff00, FALSE, FALSE, 0x0000ff00, 0x0000ff00},
        {0x7f00ff00, TRUE,  TRUE,  0x00807f00, 0x00807f00},
        {0x7f00ff00, TRUE,  FALSE, 0x0000ff00, 0x0000ff00},
        {0x7f00ff00, FALSE, TRUE,  0x00807f00, 0x00807f00},
        {0x7f00ff00, FALSE, FALSE, 0x0000ff00, 0x0000ff00},
    };

    IDirectDrawSurface7 *texture;
    DDSURFACEDESC2 surface_desc;
    IDirect3DDevice7 *device;
    IDirectDrawSurface7 *rt;
    IDirectDraw7 *ddraw;
    IDirect3D7 *d3d;
    D3DCOLOR color;
    HWND window;
    DDBLTFX fx;
    HRESULT hr;
    UINT i;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get d3d interface, hr %#x.\n", hr);
    hr = IDirect3D7_QueryInterface(d3d, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to get ddraw interface, hr %#x.\n", hr);
    IDirect3D7_Release(d3d);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CKSRCBLT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    surface_desc.dwWidth = 256;
    surface_desc.dwHeight = 256;
    U4(surface_desc).ddpfPixelFormat.dwSize = sizeof(U4(surface_desc).ddpfPixelFormat);
    U4(surface_desc).ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
    U1(U4(surface_desc).ddpfPixelFormat).dwRGBBitCount = 32;
    U2(U4(surface_desc).ddpfPixelFormat).dwRBitMask = 0x00ff0000;
    U3(U4(surface_desc).ddpfPixelFormat).dwGBitMask = 0x0000ff00;
    U4(U4(surface_desc).ddpfPixelFormat).dwBBitMask = 0x000000ff;
    U5(U4(surface_desc).ddpfPixelFormat).dwRGBAlphaBitMask = 0xff000000;
    surface_desc.ddckCKSrcBlt.dwColorSpaceLowValue = 0xff00ff00;
    surface_desc.ddckCKSrcBlt.dwColorSpaceHighValue = 0xff00ff00;
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &texture, NULL);
    ok(SUCCEEDED(hr), "Failed to create destination surface, hr %#x.\n", hr);

    hr = IDirect3DDevice7_SetTexture(device, 0, texture);
    ok(SUCCEEDED(hr), "Failed to set texture, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
    ok(SUCCEEDED(hr), "Failed to enable alpha blending, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
    ok(SUCCEEDED(hr), "Failed to enable alpha blending, hr %#x.\n", hr);

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    for (i = 0; i < sizeof(tests) / sizeof(*tests); ++i)
    {
        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_COLORKEYENABLE, tests[i].color_key);
        ok(SUCCEEDED(hr), "Failed to enable color keying, hr %#x.\n", hr);
        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ALPHABLENDENABLE, tests[i].blend);
        ok(SUCCEEDED(hr), "Failed to enable alpha blending, hr %#x.\n", hr);

        memset(&fx, 0, sizeof(fx));
        fx.dwSize = sizeof(fx);
        U5(fx).dwFillColor = tests[i].fill_color;
        hr = IDirectDrawSurface7_Blt(texture, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        ok(SUCCEEDED(hr), "Failed to fill texture, hr %#x.\n", hr);

        hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xffff0000, 1.0f, 0);
        ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
        hr = IDirect3DDevice7_BeginScene(device);
        ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
        hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_TEX1, &tquad[0], 4, 0);
        ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
        hr = IDirect3DDevice7_EndScene(device);
        ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

        color = get_surface_color(rt, 320, 240);
        if (i == 2)
            todo_wine ok(compare_color(color, tests[i].result1, 1), "Expected color 0x%08x for test %u, got 0x%08x.\n",
                    tests[i].result1, i, color);
        else
            ok(compare_color(color, tests[i].result1, 1), "Expected color 0x%08x for test %u, got 0x%08x.\n",
                    tests[i].result1, i, color);

        U5(fx).dwFillColor = 0xff0000ff;
        hr = IDirectDrawSurface7_Blt(texture, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        ok(SUCCEEDED(hr), "Failed to fill texture, hr %#x.\n", hr);

        hr = IDirect3DDevice7_BeginScene(device);
        ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
        hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_TEX1, &tquad[4], 4, 0);
        ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
        hr = IDirect3DDevice7_EndScene(device);
        ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

        /* This tests that fragments that are masked out by the color key are
         * discarded, instead of just fully transparent. */
        color = get_surface_color(rt, 320, 240);
        if (i == 2)
            todo_wine ok(compare_color(color, tests[i].result2, 1), "Expected color 0x%08x for test %u, got 0x%08x.\n",
                    tests[i].result2, i, color);
        else
            ok(compare_color(color, tests[i].result2, 1), "Expected color 0x%08x for test %u, got 0x%08x.\n",
                    tests[i].result2, i, color);
    }

    IDirectDrawSurface7_Release(rt);
    IDirectDrawSurface7_Release(texture);
    IDirectDraw7_Release(ddraw);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_ck_default(void)
{
    static struct
    {
        struct vec4 position;
        struct vec2 texcoord;
    }
    tquad[] =
    {
        {{  0.0f, 480.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{  0.0f,   0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
        {{640.0f, 480.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{640.0f,   0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
    };
    IDirectDrawSurface7 *surface, *rt;
    DDSURFACEDESC2 surface_desc;
    IDirect3DDevice7 *device;
    IDirectDraw7 *ddraw;
    IDirect3D7 *d3d;
    D3DCOLOR color;
    DWORD value;
    HWND window;
    DDBLTFX fx;
    HRESULT hr;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get d3d interface, hr %#x.\n", hr);
    hr = IDirect3D7_QueryInterface(d3d, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to get ddraw interface, hr %#x.\n", hr);
    IDirect3D7_Release(d3d);

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CKSRCBLT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    surface_desc.dwWidth = 256;
    surface_desc.dwHeight = 256;
    U4(surface_desc).ddpfPixelFormat.dwSize = sizeof(U4(surface_desc).ddpfPixelFormat);
    U4(surface_desc).ddpfPixelFormat.dwFlags = DDPF_RGB;
    U1(U4(surface_desc).ddpfPixelFormat).dwRGBBitCount = 32;
    U2(U4(surface_desc).ddpfPixelFormat).dwRBitMask = 0x00ff0000;
    U3(U4(surface_desc).ddpfPixelFormat).dwGBitMask = 0x0000ff00;
    U4(U4(surface_desc).ddpfPixelFormat).dwBBitMask = 0x000000ff;
    surface_desc.ddckCKSrcBlt.dwColorSpaceLowValue = 0x000000ff;
    surface_desc.ddckCKSrcBlt.dwColorSpaceHighValue = 0x000000ff;
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetTexture(device, 0, surface);
    ok(SUCCEEDED(hr), "Failed to set texture, hr %#x.\n", hr);

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);
    U5(fx).dwFillColor = 0x000000ff;
    hr = IDirectDrawSurface7_Blt(surface, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    ok(SUCCEEDED(hr), "Failed to fill surface, hr %#x.\n", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_COLORKEYENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!value, "Got unexpected color keying state %#x.\n", value);
    hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_TEX1, &tquad[0], 4, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);
    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x000000ff, 1), "Got unexpected color 0x%08x.\n", color);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_COLORKEYENABLE, TRUE);
    ok(SUCCEEDED(hr), "Failed to enable color keying, hr %#x.\n", hr);
    hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_TEX1, &tquad[0], 4, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_COLORKEYENABLE, &value);
    ok(SUCCEEDED(hr), "Failed to get render state, hr %#x.\n", hr);
    ok(!!value, "Got unexpected color keying state %#x.\n", value);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);
    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x0000ff00, 1), "Got unexpected color 0x%08x.\n", color);

    IDirectDrawSurface7_Release(surface);
    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    IDirectDraw7_Release(ddraw);
    DestroyWindow(window);
}

struct qi_test
{
    REFIID iid;
    REFIID refcount_iid;
    HRESULT hr;
};

static void test_qi(const char *test_name, IUnknown *base_iface,
        REFIID refcount_iid, const struct qi_test *tests, UINT entry_count)
{
    ULONG refcount, expected_refcount;
    IUnknown *iface1, *iface2;
    HRESULT hr;
    UINT i, j;

    for (i = 0; i < entry_count; ++i)
    {
        hr = IUnknown_QueryInterface(base_iface, tests[i].iid, (void **)&iface1);
        ok(hr == tests[i].hr, "Got hr %#x for test \"%s\" %u.\n", hr, test_name, i);
        if (SUCCEEDED(hr))
        {
            for (j = 0; j < entry_count; ++j)
            {
                hr = IUnknown_QueryInterface(iface1, tests[j].iid, (void **)&iface2);
                ok(hr == tests[j].hr, "Got hr %#x for test \"%s\" %u, %u.\n", hr, test_name, i, j);
                if (SUCCEEDED(hr))
                {
                    expected_refcount = 0;
                    if (IsEqualGUID(refcount_iid, tests[j].refcount_iid))
                        ++expected_refcount;
                    if (IsEqualGUID(tests[i].refcount_iid, tests[j].refcount_iid))
                        ++expected_refcount;
                    refcount = IUnknown_Release(iface2);
                    ok(refcount == expected_refcount, "Got refcount %u for test \"%s\" %u, %u, expected %u.\n",
                            refcount, test_name, i, j, expected_refcount);
                }
            }

            expected_refcount = 0;
            if (IsEqualGUID(refcount_iid, tests[i].refcount_iid))
                ++expected_refcount;
            refcount = IUnknown_Release(iface1);
            ok(refcount == expected_refcount, "Got refcount %u for test \"%s\" %u, expected %u.\n",
                    refcount, test_name, i, expected_refcount);
        }
    }
}

static void test_surface_qi(void)
{
    static const struct qi_test tests[] =
    {
        {&IID_IDirect3DTexture2,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DTexture,         NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawGammaControl,  &IID_IDirectDrawGammaControl,   S_OK         },
        {&IID_IDirectDrawColorControl,  NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface7,      &IID_IDirectDrawSurface7,       S_OK         },
        {&IID_IDirectDrawSurface4,      &IID_IDirectDrawSurface4,       S_OK         },
        {&IID_IDirectDrawSurface3,      &IID_IDirectDrawSurface3,       S_OK         },
        {&IID_IDirectDrawSurface2,      &IID_IDirectDrawSurface2,       S_OK         },
        {&IID_IDirectDrawSurface,       &IID_IDirectDrawSurface,        S_OK         },
        {&IID_IDirect3DDevice7,         NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice3,         NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice2,         NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice,          NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRampDevice,      NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRGBDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DHALDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMMXDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRefDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DTnLHalDevice,    NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DNullDevice,      NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D7,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D3,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D2,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D,                NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw7,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw4,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw3,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw2,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw,              NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DLight,           NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial2,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial3,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DExecuteBuffer,   NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport2,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport3,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DVertexBuffer,    NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DVertexBuffer7,   NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawPalette,       NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawClipper,       NULL,                           E_NOINTERFACE},
        {&IID_IUnknown,                 &IID_IDirectDrawSurface,        S_OK         },
    };

    IDirectDrawSurface7 *surface;
    DDSURFACEDESC2 surface_desc;
    IDirect3DDevice7 *device;
    IDirectDraw7 *ddraw;
    HWND window;
    HRESULT hr;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    /* Try to create a D3D device to see if the ddraw implementation supports
     * D3D. 64-bit ddraw in particular doesn't seem to support D3D, and
     * doesn't support e.g. the IDirect3DTexture interfaces. */
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }
    IDirect3DDevice_Release(device);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window);
        return;
    }
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);

    memset(&surface_desc, 0, sizeof(surface_desc));
    surface_desc.dwSize = sizeof(surface_desc);
    surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    surface_desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    surface_desc.dwWidth = 512;
    surface_desc.dwHeight = 512;
    hr = IDirectDraw7_CreateSurface(ddraw, &surface_desc, &surface, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n", hr);

    test_qi("surface_qi", (IUnknown *)surface, &IID_IDirectDrawSurface7, tests, sizeof(tests) / sizeof(*tests));

    IDirectDrawSurface7_Release(surface);
    IDirectDraw7_Release(ddraw);
    DestroyWindow(window);
}

static void test_device_qi(void)
{
    static const struct qi_test tests[] =
    {
        {&IID_IDirect3DTexture2,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DTexture,         NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawGammaControl,  NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawColorControl,  NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface7,      NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface4,      NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface3,      NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface2,      NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawSurface,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice7,         &IID_IDirect3DDevice7,          S_OK         },
        {&IID_IDirect3DDevice3,         NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice2,         NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DDevice,          NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRampDevice,      NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRGBDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DHALDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMMXDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DRefDevice,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DTnLHalDevice,    NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DNullDevice,      NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D7,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D3,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D2,               NULL,                           E_NOINTERFACE},
        {&IID_IDirect3D,                NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw7,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw4,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw3,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw2,             NULL,                           E_NOINTERFACE},
        {&IID_IDirectDraw,              NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DLight,           NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial2,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DMaterial3,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DExecuteBuffer,   NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport,        NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport2,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DViewport3,       NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DVertexBuffer,    NULL,                           E_NOINTERFACE},
        {&IID_IDirect3DVertexBuffer7,   NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawPalette,       NULL,                           E_NOINTERFACE},
        {&IID_IDirectDrawClipper,       NULL,                           E_NOINTERFACE},
        {&IID_IUnknown,                 &IID_IDirect3DDevice7,          S_OK         },
    };

    IDirect3DDevice7 *device;
    HWND window;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    test_qi("device_qi", (IUnknown *)device, &IID_IDirect3DDevice7, tests, sizeof(tests) / sizeof(*tests));

    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_wndproc(void)
{
    LONG_PTR proc, ddraw_proc;
    IDirectDraw7 *ddraw;
    WNDCLASSA wc = {0};
    HWND window;
    HRESULT hr;
    ULONG ref;

    static const UINT messages[] =
    {
        WM_WINDOWPOSCHANGING,
        WM_MOVE,
        WM_SIZE,
        WM_WINDOWPOSCHANGING,
        WM_ACTIVATE,
        WM_SETFOCUS,
        0,
    };

    /* DDSCL_EXCLUSIVE replaces the window's window proc. */
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create IDirectDraw7 object, skipping tests.\n");
        return;
    }

    wc.lpfnWndProc = test_proc;
    wc.lpszClassName = "ddraw_test_wndproc_wc";
    ok(RegisterClassA(&wc), "Failed to register window class.\n");

    window = CreateWindowA("ddraw_test_wndproc_wc", "ddraw_test",
            WS_MAXIMIZE | WS_CAPTION , 0, 0, 640, 480, 0, 0, 0, 0);

    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    expect_messages = messages;
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc != (LONG_PTR)test_proc, "Expected wndproc != %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);

    /* DDSCL_NORMAL doesn't. */
    ddraw = create_ddraw();
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);

    /* The original window proc is only restored by ddraw if the current
     * window proc matches the one ddraw set. This also affects switching
     * from DDSCL_NORMAL to DDSCL_EXCLUSIVE. */
    ddraw = create_ddraw();
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc != (LONG_PTR)test_proc, "Expected wndproc != %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    ddraw_proc = proc;
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)DefWindowProcA);
    ok(proc != (LONG_PTR)test_proc, "Expected wndproc != %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)DefWindowProcA, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)DefWindowProcA, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)ddraw_proc);
    ok(proc == (LONG_PTR)DefWindowProcA, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)DefWindowProcA, proc);
    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);

    ddraw = create_ddraw();
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)test_proc, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    proc = SetWindowLongPtrA(window, GWLP_WNDPROC, (LONG_PTR)DefWindowProcA);
    ok(proc != (LONG_PTR)test_proc, "Expected wndproc != %#lx, got %#lx.\n",
            (LONG_PTR)test_proc, proc);
    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    proc = GetWindowLongPtrA(window, GWLP_WNDPROC);
    ok(proc == (LONG_PTR)DefWindowProcA, "Expected wndproc %#lx, got %#lx.\n",
            (LONG_PTR)DefWindowProcA, proc);

    fix_wndproc(window, (LONG_PTR)test_proc);
    expect_messages = NULL;
    DestroyWindow(window);
    UnregisterClassA("ddraw_test_wndproc_wc", GetModuleHandleA(NULL));
}

static void test_window_style(void)
{
    LONG style, exstyle, tmp;
    RECT fullscreen_rect, r;
    IDirectDraw7 *ddraw;
    HWND window;
    HRESULT hr;
    ULONG ref;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, 0, 0, 0, 0);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    style = GetWindowLongA(window, GWL_STYLE);
    exstyle = GetWindowLongA(window, GWL_EXSTYLE);
    SetRect(&fullscreen_rect, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    tmp = GetWindowLongA(window, GWL_STYLE);
    todo_wine ok(tmp == style, "Expected window style %#x, got %#x.\n", style, tmp);
    tmp = GetWindowLongA(window, GWL_EXSTYLE);
    todo_wine ok(tmp == exstyle, "Expected window extended style %#x, got %#x.\n", exstyle, tmp);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);
    GetClientRect(window, &r);
    todo_wine ok(!EqualRect(&r, &fullscreen_rect), "Client rect and window rect are equal.\n");

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    tmp = GetWindowLongA(window, GWL_STYLE);
    todo_wine ok(tmp == style, "Expected window style %#x, got %#x.\n", style, tmp);
    tmp = GetWindowLongA(window, GWL_EXSTYLE);
    todo_wine ok(tmp == exstyle, "Expected window extended style %#x, got %#x.\n", exstyle, tmp);

    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);

    DestroyWindow(window);
}

static void test_redundant_mode_set(void)
{
    DDSURFACEDESC2 surface_desc = {0};
    IDirectDraw7 *ddraw;
    HWND window;
    HRESULT hr;
    RECT r, s;
    ULONG ref;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, 0, 0, 0, 0);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    surface_desc.dwSize = sizeof(surface_desc);
    hr = IDirectDraw7_GetDisplayMode(ddraw, &surface_desc);
    ok(SUCCEEDED(hr), "GetDipslayMode failed, hr %#x.\n", hr);

    hr = IDirectDraw7_SetDisplayMode(ddraw, surface_desc.dwWidth, surface_desc.dwHeight,
            U1(U4(surface_desc).ddpfPixelFormat).dwRGBBitCount, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    GetWindowRect(window, &r);
    r.right /= 2;
    r.bottom /= 2;
    SetWindowPos(window, HWND_TOP, r.left, r.top, r.right, r.bottom, 0);
    GetWindowRect(window, &s);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            r.left, r.top, r.right, r.bottom,
            s.left, s.top, s.right, s.bottom);

    hr = IDirectDraw7_SetDisplayMode(ddraw, surface_desc.dwWidth, surface_desc.dwHeight,
            U1(U4(surface_desc).ddpfPixelFormat).dwRGBBitCount, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    GetWindowRect(window, &s);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            r.left, r.top, r.right, r.bottom,
            s.left, s.top, s.right, s.bottom);

    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);

    DestroyWindow(window);
}

static SIZE screen_size, screen_size2;

static LRESULT CALLBACK mode_set_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_SIZE)
    {
        screen_size.cx = GetSystemMetrics(SM_CXSCREEN);
        screen_size.cy = GetSystemMetrics(SM_CYSCREEN);
    }

    return test_proc(hwnd, message, wparam, lparam);
}

static LRESULT CALLBACK mode_set_proc2(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_SIZE)
    {
        screen_size2.cx = GetSystemMetrics(SM_CXSCREEN);
        screen_size2.cy = GetSystemMetrics(SM_CYSCREEN);
    }

    return test_proc(hwnd, message, wparam, lparam);
}

static void test_coop_level_mode_set(void)
{
    IDirectDrawSurface7 *primary;
    RECT fullscreen_rect, r, s;
    IDirectDraw7 *ddraw;
    DDSURFACEDESC2 ddsd;
    WNDCLASSA wc = {0};
    HWND window, window2;
    HRESULT hr;
    ULONG ref;

    static const UINT exclusive_messages[] =
    {
        WM_WINDOWPOSCHANGING,
        WM_WINDOWPOSCHANGED,
        WM_SIZE,
        WM_DISPLAYCHANGE,
        0,
    };

    static const UINT normal_messages[] =
    {
        WM_DISPLAYCHANGE,
        0,
    };

    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        return;
    }

    wc.lpfnWndProc = mode_set_proc;
    wc.lpszClassName = "ddraw_test_wndproc_wc";
    ok(RegisterClassA(&wc), "Failed to register window class.\n");
    wc.lpfnWndProc = mode_set_proc2;
    wc.lpszClassName = "ddraw_test_wndproc_wc2";
    ok(RegisterClassA(&wc), "Failed to register window class.\n");

    window = CreateWindowA("ddraw_test_wndproc_wc", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, 0, 0, 0, 0);
    window2 = CreateWindowA("ddraw_test_wndproc_wc2", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, 0, 0, 0, 0);

    SetRect(&fullscreen_rect, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    SetRect(&s, 0, 0, 640, 480);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = exclusive_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(screen_size.cx == s.right && screen_size.cy == s.bottom,
            "Expected screen size %ux%u, got %ux%u.\n",
            s.right, s.bottom, screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            s.left, s.top, s.right, s.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            s.left, s.top, s.right, s.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = exclusive_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw_RestoreDisplayMode(ddraw);
    ok(SUCCEEDED(hr), "RestoreDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(screen_size.cx == fullscreen_rect.right && screen_size.cy == fullscreen_rect.bottom,
            "Expected screen size %ux%u, got %ux%u.\n",
            fullscreen_rect.right, fullscreen_rect.bottom, screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = normal_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(!screen_size.cx && !screen_size.cy, "Got unxpected screen size %ux%u.\n", screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = normal_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw_RestoreDisplayMode(ddraw);
    ok(SUCCEEDED(hr), "RestoreDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(!screen_size.cx && !screen_size.cy, "Got unxpected screen size %ux%u.\n", screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    /* DDSCL_NORMAL | DDSCL_FULLSCREEN behaves the same as just DDSCL_NORMAL.
     * Resizing the window on mode changes is a property of DDSCL_EXCLUSIVE,
     * not DDSCL_FULLSCREEN. */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = normal_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(!screen_size.cx && !screen_size.cy, "Got unxpected screen size %ux%u.\n", screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = normal_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw_RestoreDisplayMode(ddraw);
    ok(SUCCEEDED(hr), "RestoreDisplayMode failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(!screen_size.cx && !screen_size.cy, "Got unxpected screen size %ux%u.\n", screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    /* Changing the coop level from EXCLUSIVE to NORMAL restores the screen resolution */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    expect_messages = exclusive_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(screen_size.cx == fullscreen_rect.right && screen_size.cy == fullscreen_rect.bottom,
            "Expected screen size %ux%u, got %ux%u.\n",
            fullscreen_rect.right, fullscreen_rect.bottom, screen_size.cx, screen_size.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    /* The screen restore is a property of DDSCL_EXCLUSIVE  */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == s.right - s.left, "Expected surface width %u, got %u.\n",
            s.right - s.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == s.bottom - s.top, "Expected surface height %u, got %u.\n",
            s.bottom - s.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    hr = IDirectDraw_RestoreDisplayMode(ddraw);
    ok(SUCCEEDED(hr), "RestoreDisplayMode failed, hr %#x.\n", hr);

    /* If the window is changed at the same time, messages are sent to the new window. */
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);

    expect_messages = exclusive_messages;
    screen_size.cx = 0;
    screen_size.cy = 0;
    screen_size2.cx = 0;
    screen_size2.cy = 0;

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window2, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    ok(!*expect_messages, "Expected message %#x, but didn't receive it.\n", *expect_messages);
    expect_messages = NULL;
    ok(!screen_size.cx && !screen_size.cy, "Got unxpected screen size %ux%u.\n",
            screen_size.cx, screen_size.cy);
    ok(screen_size2.cx == fullscreen_rect.right && screen_size2.cy == fullscreen_rect.bottom,
            "Expected screen size 2 %ux%u, got %ux%u.\n",
            fullscreen_rect.right, fullscreen_rect.bottom, screen_size2.cx, screen_size2.cy);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            s.left, s.top, s.right, s.bottom,
            r.left, r.top, r.right, r.bottom);
    GetWindowRect(window2, &r);
    ok(EqualRect(&r, &fullscreen_rect), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            fullscreen_rect.left, fullscreen_rect.top, fullscreen_rect.right, fullscreen_rect.bottom,
            r.left, r.top, r.right, r.bottom);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &primary, NULL);
    ok(SUCCEEDED(hr), "Failed to create surface, hr %#x.\n",hr);
    hr = IDirectDrawSurface7_GetSurfaceDesc(primary, &ddsd);
    ok(SUCCEEDED(hr), "Failed to get surface desc, hr %#x.\n", hr);
    ok(ddsd.dwWidth == fullscreen_rect.right - fullscreen_rect.left, "Expected surface width %u, got %u.\n",
            fullscreen_rect.right - fullscreen_rect.left, ddsd.dwWidth);
    ok(ddsd.dwHeight == fullscreen_rect.bottom - fullscreen_rect.top, "Expected surface height %u, got %u.\n",
            fullscreen_rect.bottom - fullscreen_rect.top, ddsd.dwHeight);
    IDirectDrawSurface7_Release(primary);

    ref = IDirectDraw7_Release(ddraw);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);

    GetWindowRect(window, &r);
    ok(EqualRect(&r, &s), "Expected {%d, %d, %d, %d}, got {%d, %d, %d, %d}.\n",
            s.left, s.top, s.right, s.bottom,
            r.left, r.top, r.right, r.bottom);

    expect_messages = NULL;
    DestroyWindow(window);
    DestroyWindow(window2);
    UnregisterClassA("ddraw_test_wndproc_wc", GetModuleHandleA(NULL));
    UnregisterClassA("ddraw_test_wndproc_wc2", GetModuleHandleA(NULL));
}

static void test_coop_level_mode_set_multi(void)
{
    IDirectDraw7 *ddraw1, *ddraw2;
    UINT orig_w, orig_h, w, h;
    HWND window;
    HRESULT hr;
    ULONG ref;

    if (!(ddraw1 = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        return;
    }

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, 0, 0, 0, 0);

    orig_w = GetSystemMetrics(SM_CXSCREEN);
    orig_h = GetSystemMetrics(SM_CYSCREEN);

    /* With just a single ddraw object, the display mode is restored on
     * release. */
    hr = IDirectDraw7_SetDisplayMode(ddraw1, 800, 600, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 800, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 600, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    /* When there are multiple ddraw objects, the display mode is restored to
     * the initial mode, before the first SetDisplayMode() call. */
    ddraw1 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw1, 800, 600, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 800, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 600, "Got unexpected screen height %u.\n", h);

    ddraw2 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw2, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw2);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    /* Regardless of release ordering. */
    ddraw1 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw1, 800, 600, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 800, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 600, "Got unexpected screen height %u.\n", h);

    ddraw2 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw2, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw2);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    /* But only for ddraw objects that called SetDisplayMode(). */
    ddraw1 = create_ddraw();
    ddraw2 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw2, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw2);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    /* If there's a ddraw object that's currently in exclusive mode, it blocks
     * restoring the display mode. */
    ddraw1 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw1, 800, 600, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 800, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 600, "Got unexpected screen height %u.\n", h);

    ddraw2 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw2, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw2, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 640, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 480, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw2);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    /* Exclusive mode blocks mode setting on other ddraw objects in general. */
    ddraw1 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw1, 800, 600, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == 800, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == 600, "Got unexpected screen height %u.\n", h);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw1, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    ddraw2 = create_ddraw();
    hr = IDirectDraw7_SetDisplayMode(ddraw2, 640, 480, 32, 0, 0);
    ok(hr == DDERR_NOEXCLUSIVEMODE, "Got unexpected hr %#x.\n", hr);

    ref = IDirectDraw7_Release(ddraw1);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    ref = IDirectDraw7_Release(ddraw2);
    ok(ref == 0, "The ddraw object was not properly freed: refcount %u.\n", ref);
    w = GetSystemMetrics(SM_CXSCREEN);
    ok(w == orig_w, "Got unexpected screen width %u.\n", w);
    h = GetSystemMetrics(SM_CYSCREEN);
    ok(h == orig_h, "Got unexpected screen height %u.\n", h);

    DestroyWindow(window);
}

static void test_initialize(void)
{
    IDirectDraw7 *ddraw;
    HRESULT hr;

    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        return;
    }

    hr = IDirectDraw7_Initialize(ddraw, NULL);
    ok(hr == DDERR_ALREADYINITIALIZED, "Initialize returned hr %#x.\n", hr);
    IDirectDraw7_Release(ddraw);

    CoInitialize(NULL);
    hr = CoCreateInstance(&CLSID_DirectDraw, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectDraw7, (void **)&ddraw);
    ok(SUCCEEDED(hr), "Failed to create IDirectDraw7 instance, hr %#x.\n", hr);
    hr = IDirectDraw7_Initialize(ddraw, NULL);
    ok(hr == DD_OK, "Initialize returned hr %#x, expected DD_OK.\n", hr);
    hr = IDirectDraw7_Initialize(ddraw, NULL);
    ok(hr == DDERR_ALREADYINITIALIZED, "Initialize returned hr %#x, expected DDERR_ALREADYINITIALIZED.\n", hr);
    IDirectDraw7_Release(ddraw);
    CoUninitialize();
}

static void test_coop_level_surf_create(void)
{
    IDirectDrawSurface7 *surface;
    IDirectDraw7 *ddraw;
    DDSURFACEDESC2 ddsd;
    HRESULT hr;

    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        return;
    }

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    hr = IDirectDraw7_CreateSurface(ddraw, &ddsd, &surface, NULL);
    ok(hr == DDERR_NOCOOPERATIVELEVELSET, "Surface creation returned hr %#x.\n", hr);

    IDirectDraw7_Release(ddraw);
}

static void test_vb_discard(void)
{
    static const struct vec4 quad[] =
    {
        {  0.0f, 480.0f, 0.0f, 1.0f},
        {  0.0f,   0.0f, 0.0f, 1.0f},
        {640.0f, 480.0f, 0.0f, 1.0f},
        {640.0f,   0.0f, 0.0f, 1.0f},
    };

    IDirect3DDevice7 *device;
    IDirect3D7 *d3d;
    IDirect3DVertexBuffer7 *buffer;
    HWND window;
    HRESULT hr;
    D3DVERTEXBUFFERDESC desc;
    BYTE *data;
    static const unsigned int vbsize = 16;
    unsigned int i;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetDirect3D(device, &d3d);
    ok(SUCCEEDED(hr), "Failed to get d3d interface, hr %#x.\n", hr);

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwCaps = D3DVBCAPS_WRITEONLY;
    desc.dwFVF = D3DFVF_XYZRHW;
    desc.dwNumVertices = vbsize;
    hr = IDirect3D7_CreateVertexBuffer(d3d, &desc, &buffer, 0);
    ok(SUCCEEDED(hr), "Failed to create vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(buffer, DDLOCK_DISCARDCONTENTS, (void **)&data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock vertex buffer, hr %#x.\n", hr);
    memcpy(data, quad, sizeof(quad));
    hr = IDirect3DVertexBuffer7_Unlock(buffer);
    ok(SUCCEEDED(hr), "Failed to unlock vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
    hr = IDirect3DDevice7_DrawPrimitiveVB(device, D3DPT_TRIANGLESTRIP, buffer, 0, 4, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(buffer, DDLOCK_DISCARDCONTENTS, (void **)&data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock vertex buffer, hr %#x.\n", hr);
    memset(data, 0xaa, sizeof(struct vec4) * vbsize);
    hr = IDirect3DVertexBuffer7_Unlock(buffer);
    ok(SUCCEEDED(hr), "Failed to unlock vertex buffer, hr %#x.\n", hr);

    hr = IDirect3DVertexBuffer7_Lock(buffer, DDLOCK_DISCARDCONTENTS, (void **)&data, NULL);
    ok(SUCCEEDED(hr), "Failed to lock vertex buffer, hr %#x.\n", hr);
    for (i = 0; i < sizeof(struct vec4) * vbsize; i++)
    {
        if (data[i] != 0xaa)
        {
            ok(FALSE, "Vertex buffer data byte %u is 0x%02x, expected 0xaa\n", i, data[i]);
            break;
        }
    }
    hr = IDirect3DVertexBuffer7_Unlock(buffer);
    ok(SUCCEEDED(hr), "Failed to unlock vertex buffer, hr %#x.\n", hr);

    IDirect3DVertexBuffer7_Release(buffer);
    IDirect3D7_Release(d3d);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_coop_level_multi_window(void)
{
    HWND window1, window2;
    IDirectDraw7 *ddraw;
    HRESULT hr;

    window1 = CreateWindowA("static", "ddraw_test1", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    window2 = CreateWindowA("static", "ddraw_test2", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(ddraw = create_ddraw()))
    {
        skip("Failed to create a ddraw object, skipping test.\n");
        DestroyWindow(window2);
        DestroyWindow(window1);
        return;
    }

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window1, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window2, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "Failed to set cooperative level, hr %#x.\n", hr);
    ok(IsWindow(window1), "Window 1 was destroyed.\n");
    ok(IsWindow(window2), "Window 2 was destroyed.\n");

    IDirectDraw7_Release(ddraw);
    DestroyWindow(window2);
    DestroyWindow(window1);
}

static void test_draw_strided(void)
{
    static struct vec3 position[] =
    {
        {-1.0,   -1.0,   0.0},
        {-1.0,    1.0,   0.0},
        { 1.0,    1.0,   0.0},
        { 1.0,   -1.0,   0.0},
    };
    static DWORD diffuse[] =
    {
        0x0000ff00, 0x0000ff00, 0x0000ff00, 0x0000ff00,
    };
    static WORD indices[] =
    {
        0, 1, 2, 2, 3, 0
    };

    IDirectDrawSurface7 *rt;
    IDirect3DDevice7 *device;
    D3DCOLOR color;
    HWND window;
    HRESULT hr;
    D3DDRAWPRIMITIVESTRIDEDDATA strided;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    ok(SUCCEEDED(hr), "Failed to disable lighting, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_BeginScene(device);
    ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);

    memset(&strided, 0x55, sizeof(strided));
    strided.position.lpvData = position;
    strided.position.dwStride = sizeof(*position);
    strided.diffuse.lpvData = diffuse;
    strided.diffuse.dwStride = sizeof(*diffuse);
    hr = IDirect3DDevice7_DrawIndexedPrimitiveStrided(device, D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE,
            &strided, 4, indices, 6, 0);
    ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);

    hr = IDirect3DDevice7_EndScene(device);
    ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x0000ff00, 1), "Got unexpected color 0x%08x.\n", color);

    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_clear_rect_count(void)
{
    IDirectDrawSurface7 *rt;
    IDirect3DDevice7 *device;
    D3DCOLOR color;
    HWND window;
    HRESULT hr;
    D3DRECT rect = {{0}, {0}, {640}, {480}};

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);
    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00ffffff, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Clear(device, 0, &rect, D3DCLEAR_TARGET, 0x00ff0000, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);

    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x00ffffff, 1),
            "Clear with count = 0, rect != NULL has color %#08x.\n", color);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00ffffff, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);
    hr = IDirect3DDevice7_Clear(device, 1, NULL, D3DCLEAR_TARGET, 0x0000ff00, 1.0f, 0);
    ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);

    color = get_surface_color(rt, 320, 240);
    ok(compare_color(color, 0x0000ff00, 1),
            "Clear with count = 1, rect = NULL has color %#08x.\n", color);

    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static BOOL test_mode_restored(IDirectDraw7 *ddraw, HWND window)
{
    DDSURFACEDESC2 ddsd1, ddsd2;
    HRESULT hr;

    memset(&ddsd1, 0, sizeof(ddsd1));
    ddsd1.dwSize = sizeof(ddsd1);
    hr = IDirectDraw7_GetDisplayMode(ddraw, &ddsd1);
    ok(SUCCEEDED(hr), "GetDisplayMode failed, hr %#x.\n", hr);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetDisplayMode(ddraw, 640, 480, 32, 0, 0);
    ok(SUCCEEDED(hr), "SetDisplayMode failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    memset(&ddsd2, 0, sizeof(ddsd2));
    ddsd2.dwSize = sizeof(ddsd2);
    hr = IDirectDraw7_GetDisplayMode(ddraw, &ddsd2);
    ok(SUCCEEDED(hr), "GetDisplayMode failed, hr %#x.\n", hr);
    hr = IDirectDraw7_RestoreDisplayMode(ddraw);
    ok(SUCCEEDED(hr), "RestoreDisplayMode failed, hr %#x.\n", hr);

    return ddsd1.dwWidth == ddsd2.dwWidth && ddsd1.dwHeight == ddsd2.dwHeight;
}

static void test_coop_level_versions(void)
{
    HWND window;
    IDirectDraw *ddraw;
    HRESULT hr;
    BOOL restored;
    IDirectDrawSurface *surface;
    IDirectDraw7 *ddraw7;
    DDSURFACEDESC ddsd;

    window = CreateWindowA("static", "ddraw_test1", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(ddraw7 = create_ddraw()))
        goto done;
    /* Newly created ddraw objects restore the mode on ddraw2+::SetCooperativeLevel(NORMAL) */
    restored = test_mode_restored(ddraw7, window);
    ok(restored, "Display mode not restored in new ddraw object\n");

    /* A failing ddraw1::SetCooperativeLevel call does not have an effect */
    hr = IDirectDraw7_QueryInterface(ddraw7, &IID_IDirectDraw, (void **)&ddraw);
    ok(SUCCEEDED(hr), "QueryInterface failed, hr %#x.\n", hr);

    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE);
    ok(FAILED(hr), "SetCooperativeLevel returned %#x, expected failure.\n", hr);
    restored = test_mode_restored(ddraw7, window);
    ok(restored, "Display mode not restored after bad ddraw1::SetCooperativeLevel call\n");

    /* A successful one does */
    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    restored = test_mode_restored(ddraw7, window);
    ok(!restored, "Display mode restored after good ddraw1::SetCooperativeLevel call\n");

    IDirectDraw_Release(ddraw);
    IDirectDraw7_Release(ddraw7);

    if (!(ddraw7 = create_ddraw()))
        goto done;
    hr = IDirectDraw7_QueryInterface(ddraw7, &IID_IDirectDraw, (void **)&ddraw);
    ok(SUCCEEDED(hr), "QueryInterface failed, hr %#x.\n", hr);

    hr = IDirectDraw_SetCooperativeLevel(ddraw, window, DDSCL_SETFOCUSWINDOW);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    restored = test_mode_restored(ddraw7, window);
    ok(!restored, "Display mode restored after ddraw1::SetCooperativeLevel(SETFOCUSWINDOW) call\n");

    IDirectDraw_Release(ddraw);
    IDirectDraw7_Release(ddraw7);

    /* A failing call does not restore the ddraw2+ behavior */
    if (!(ddraw7 = create_ddraw()))
        goto done;
    hr = IDirectDraw7_QueryInterface(ddraw7, &IID_IDirectDraw, (void **)&ddraw);
    ok(SUCCEEDED(hr), "QueryInterface failed, hr %#x.\n", hr);

    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE);
    ok(FAILED(hr), "SetCooperativeLevel returned %#x, expected failure.\n", hr);
    restored = test_mode_restored(ddraw7, window);
    ok(!restored, "Display mode restored after good-bad ddraw1::SetCooperativeLevel() call sequence\n");

    IDirectDraw_Release(ddraw);
    IDirectDraw7_Release(ddraw7);

    /* Neither does a sequence of successful calls with the new interface */
    if (!(ddraw7 = create_ddraw()))
        goto done;
    hr = IDirectDraw7_QueryInterface(ddraw7, &IID_IDirectDraw, (void **)&ddraw);
    ok(SUCCEEDED(hr), "QueryInterface failed, hr %#x.\n", hr);

    hr = IDirectDraw_SetCooperativeLevel(ddraw, NULL, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw7, window, DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);
    hr = IDirectDraw7_SetCooperativeLevel(ddraw7, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    restored = test_mode_restored(ddraw7, window);
    ok(!restored, "Display mode restored after ddraw1-ddraw7 SetCooperativeLevel() call sequence\n");
    IDirectDraw_Release(ddraw);
    IDirectDraw7_Release(ddraw7);

    /* ddraw1::CreateSurface does not triger the ddraw1 behavior */
    if (!(ddraw7 = create_ddraw()))
        goto done;
    hr = IDirectDraw7_QueryInterface(ddraw7, &IID_IDirectDraw, (void **)&ddraw);
    ok(SUCCEEDED(hr), "QueryInterface failed, hr %#x.\n", hr);

    hr = IDirectDraw7_SetCooperativeLevel(ddraw7, window, DDSCL_NORMAL);
    ok(SUCCEEDED(hr), "SetCooperativeLevel failed, hr %#x.\n", hr);

    memset(&ddsd, 0, sizeof(ddsd));
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    ddsd.dwWidth = ddsd.dwHeight = 8;
    hr = IDirectDraw_CreateSurface(ddraw, &ddsd, &surface, NULL);
    ok(SUCCEEDED(hr), "CreateSurface failed, hr %#x.\n", hr);
    IDirectDrawSurface_Release(surface);
    restored = test_mode_restored(ddraw7, window);
    ok(restored, "Display mode not restored after ddraw1::CreateSurface() call\n");

    IDirectDraw_Release(ddraw);
    IDirectDraw7_Release(ddraw7);

done:
    DestroyWindow(window);
}

static void test_fog_special(void)
{
    static struct
    {
        struct vec3 position;
        D3DCOLOR diffuse;
    }
    quad[] =
    {
        {{ -1.0f,   1.0f,  0.0f}, 0xff00ff00},
        {{  1.0f,   1.0f,  1.0f}, 0xff00ff00},
        {{ -1.0f,  -1.0f,  0.0f}, 0xff00ff00},
        {{  1.0f,  -1.0f,  1.0f}, 0xff00ff00},
    };
    static const struct
    {
        DWORD vertexmode, tablemode;
        D3DCOLOR color_left, color_right;
    }
    tests[] =
    {
        {D3DFOG_LINEAR, D3DFOG_NONE,    0x00ff0000, 0x00ff0000},
        {D3DFOG_NONE,   D3DFOG_LINEAR,  0x0000ff00, 0x00ff0000},
    };
    union
    {
        float f;
        DWORD d;
    } conv;
    D3DCOLOR color;
    HRESULT hr;
    unsigned int i;
    HWND window;
    IDirect3DDevice7 *device;
    IDirectDrawSurface7 *rt;

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGENABLE, TRUE);
    ok(SUCCEEDED(hr), "Failed to enable fog, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGCOLOR, 0xffff0000);
    ok(SUCCEEDED(hr), "Failed to set fog color, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    ok(SUCCEEDED(hr), "Failed to disable lighting, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    ok(SUCCEEDED(hr), "Failed to disable lighting, hr %#x.\n", hr);

    conv.f = 0.5f;
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGSTART, conv.d);
    ok(SUCCEEDED(hr), "Failed to set fog start, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGEND, conv.d);
    ok(SUCCEEDED(hr), "Failed to set fog end, hr %#x.\n", hr);

    for (i = 0; i < sizeof(tests) / sizeof(*tests); i++)
    {
        hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x000000ff, 1.0f, 0);
        ok(SUCCEEDED(hr), "Failed to clear render target, hr %#x.\n", hr);

        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGVERTEXMODE, tests[i].vertexmode);
        ok(SUCCEEDED(hr), "Failed to set fogvertexmode, hr %#x.\n", hr);
        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGTABLEMODE, tests[i].tablemode);
        ok(SUCCEEDED(hr), "Failed to set fogtablemode, hr %#x.\n", hr);

        hr = IDirect3DDevice7_BeginScene(device);
        ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
        hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP, D3DFVF_XYZ | D3DFVF_DIFFUSE, quad, 4, 0);
        ok(SUCCEEDED(hr), "Failed to draw, hr %#x.\n", hr);
        hr = IDirect3DDevice7_EndScene(device);
        ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

        color = get_surface_color(rt, 310, 240);
        ok(compare_color(color, tests[i].color_left, 1),
                "Expected left color 0x%08x, got 0x%08x, case %u.\n", tests[i].color_left, color, i);
        color = get_surface_color(rt, 330, 240);
        ok(compare_color(color, tests[i].color_right, 1),
                "Expected right color 0x%08x, got 0x%08x, case %u.\n", tests[i].color_right, color, i);
    }

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_FOGENABLE, FALSE);
    ok(SUCCEEDED(hr), "Failed to disable fog, hr %#x.\n", hr);

    IDirectDrawSurface7_Release(rt);
    IDirect3DDevice7_Release(device);
    DestroyWindow(window);
}

static void test_lighting_interface_versions(void)
{
    IDirect3DDevice7 *device;
    IDirectDrawSurface7 *rt;
    D3DCOLOR color;
    HWND window;
    HRESULT hr;
    DWORD rs;
    unsigned int i;
    ULONG ref;
    D3DMATERIAL7 material;
    static D3DVERTEX quad[] =
    {
        {{-1.0f}, { 1.0f}, {0.0f}, {1.0f}, {0.0f}, {0.0f}},
        {{ 1.0f}, { 1.0f}, {0.0f}, {1.0f}, {0.0f}, {0.0f}},
        {{-1.0f}, {-1.0f}, {0.0f}, {1.0f}, {0.0f}, {0.0f}},
        {{ 1.0f}, {-1.0f}, {0.0f}, {1.0f}, {0.0f}, {0.0f}},
    };

#define FVF_COLORVERTEX (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)
    static struct
    {
        struct vec3 position;
        struct vec3 normal;
        DWORD diffuse, specular;
    }
    quad2[] =
    {
        {{-1.0f,  1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{ 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{-1.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{ 1.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 0xffff0000, 0xff808080},
    };

    static D3DLVERTEX lquad[] =
    {
        {{-1.0f}, { 1.0f}, {0.0f}, 0, {0xffff0000}, {0xff808080}},
        {{ 1.0f}, { 1.0f}, {0.0f}, 0, {0xffff0000}, {0xff808080}},
        {{-1.0f}, {-1.0f}, {0.0f}, 0, {0xffff0000}, {0xff808080}},
        {{ 1.0f}, {-1.0f}, {0.0f}, 0, {0xffff0000}, {0xff808080}},
    };

#define FVF_LVERTEX2 (D3DFVF_LVERTEX & ~D3DFVF_RESERVED1)
    static struct
    {
        struct vec3 position;
        DWORD diffuse, specular;
        struct vec2 texcoord;
    }
    lquad2[] =
    {
        {{-1.0f,  1.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{ 1.0f,  1.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{-1.0f, -1.0f, 0.0f}, 0xffff0000, 0xff808080},
        {{ 1.0f, -1.0f, 0.0f}, 0xffff0000, 0xff808080},
    };

    static D3DTLVERTEX tlquad[] =
    {
        {{   0.0f}, { 480.0f}, {0.0f}, {1.0f}, {0xff0000ff}, {0xff808080}},
        {{   0.0f}, {   0.0f}, {0.0f}, {1.0f}, {0xff0000ff}, {0xff808080}},
        {{ 640.0f}, { 480.0f}, {0.0f}, {1.0f}, {0xff0000ff}, {0xff808080}},
        {{ 640.0f}, {   0.0f}, {0.0f}, {1.0f}, {0xff0000ff}, {0xff808080}},
    };

#define FVF_TLVERTEX2 (D3DFVF_XYZRHW | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR)
    static struct
    {
        struct vec4 position;
        struct vec3 normal;
        DWORD diffuse, specular;
    }
    tlquad2[] =
    {
        {{   0.0f,  480.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0xff0000ff, 0xff808080},
        {{   0.0f,    0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0xff0000ff, 0xff808080},
        {{ 640.0f,  480.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0xff0000ff, 0xff808080},
        {{ 640.0f,    0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, 0xff0000ff, 0xff808080},
    };


    static const struct
    {
        DWORD vertextype;
        void *data;
        DWORD d3drs_lighting, d3drs_specular;
        DWORD draw_flags;
        D3DCOLOR color;
    }
    tests[] =
    {
        /* Lighting is enabled when D3DFVF_XYZ is used and D3DRENDERSTATE_LIGHTING is
         * enabled. D3DDP_DONOTLIGHT is ignored. Lighting is also enabled when normals
         * are not available
         *
         * Note that the specular result is 0x00000000 when lighting is on even if the
         * input vertex has specular color because D3DRENDERSTATE_COLORVERTEX is not
         * enabled */

        /* 0 */
        { D3DFVF_VERTEX,    quad,       FALSE,  FALSE,  0,                  0x00ffffff},
        { D3DFVF_VERTEX,    quad,       TRUE,   FALSE,  0,                  0x0000ff00},
        { D3DFVF_VERTEX,    quad,       FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x00ffffff},
        { D3DFVF_VERTEX,    quad,       TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x0000ff00},
        { D3DFVF_VERTEX,    quad,       FALSE,  TRUE,   0,                  0x00ffffff},
        { D3DFVF_VERTEX,    quad,       TRUE,   TRUE,   0,                  0x0000ff00},
        { D3DFVF_VERTEX,    quad,       FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x00ffffff},
        { D3DFVF_VERTEX,    quad,       TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x0000ff00},

        /* 8 */
        { FVF_COLORVERTEX,  quad2,      FALSE,  FALSE,  0,                  0x00ff0000},
        { FVF_COLORVERTEX,  quad2,      TRUE,   FALSE,  0,                  0x0000ff00},
        { FVF_COLORVERTEX,  quad2,      FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x00ff0000},
        { FVF_COLORVERTEX,  quad2,      TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x0000ff00},
        { FVF_COLORVERTEX,  quad2,      FALSE,  TRUE,   0,                  0x00ff8080},
        { FVF_COLORVERTEX,  quad2,      TRUE,   TRUE,   0,                  0x0000ff00},
        { FVF_COLORVERTEX,  quad2,      FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x00ff8080},
        { FVF_COLORVERTEX,  quad2,      TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x0000ff00},

        /* 16 */
        { D3DFVF_LVERTEX,   lquad,      FALSE,  FALSE,  0,                  0x00ff0000},
        { D3DFVF_LVERTEX,   lquad,      TRUE,   FALSE,  0,                  0x0000ff00},
        { D3DFVF_LVERTEX,   lquad,      FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x00ff0000},
        { D3DFVF_LVERTEX,   lquad,      TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x0000ff00},
        { D3DFVF_LVERTEX,   lquad,      FALSE,  TRUE,   0,                  0x00ff8080},
        { D3DFVF_LVERTEX,   lquad,      TRUE,   TRUE,   0,                  0x0000ff00},
        { D3DFVF_LVERTEX,   lquad,      FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x00ff8080},
        { D3DFVF_LVERTEX,   lquad,      TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x0000ff00},

        /* 24 */
        { FVF_LVERTEX2,     lquad2,     FALSE,  FALSE,  0,                  0x00ff0000},
        { FVF_LVERTEX2,     lquad2,     TRUE,   FALSE,  0,                  0x0000ff00},
        { FVF_LVERTEX2,     lquad2,     FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x00ff0000},
        { FVF_LVERTEX2,     lquad2,     TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x0000ff00},
        { FVF_LVERTEX2,     lquad2,     FALSE,  TRUE,   0,                  0x00ff8080},
        { FVF_LVERTEX2,     lquad2,     TRUE,   TRUE,   0,                  0x0000ff00},
        { FVF_LVERTEX2,     lquad2,     FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x00ff8080},
        { FVF_LVERTEX2,     lquad2,     TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x0000ff00},

        /* 32 */
        { D3DFVF_TLVERTEX,  tlquad,     FALSE,  FALSE,  0,                  0x000000ff},
        { D3DFVF_TLVERTEX,  tlquad,     TRUE,   FALSE,  0,                  0x000000ff},
        { D3DFVF_TLVERTEX,  tlquad,     FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x000000ff},
        { D3DFVF_TLVERTEX,  tlquad,     TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x000000ff},
        { D3DFVF_TLVERTEX,  tlquad,     FALSE,  TRUE,   0,                  0x008080ff},
        { D3DFVF_TLVERTEX,  tlquad,     TRUE,   TRUE,   0,                  0x008080ff},
        { D3DFVF_TLVERTEX,  tlquad,     FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x008080ff},
        { D3DFVF_TLVERTEX,  tlquad,     TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x008080ff},

        /* 40 */
        { FVF_TLVERTEX2,    tlquad2,    FALSE,  FALSE,  0,                  0x000000ff},
        { FVF_TLVERTEX2,    tlquad2,    TRUE,   FALSE,  0,                  0x000000ff},
        { FVF_TLVERTEX2,    tlquad2,    FALSE,  FALSE,  D3DDP_DONOTLIGHT,   0x000000ff},
        { FVF_TLVERTEX2,    tlquad2,    TRUE,   FALSE,  D3DDP_DONOTLIGHT,   0x000000ff},
        { FVF_TLVERTEX2,    tlquad2,    FALSE,  TRUE,   0,                  0x008080ff},
        { FVF_TLVERTEX2,    tlquad2,    TRUE,   TRUE,   0,                  0x008080ff},
        { FVF_TLVERTEX2,    tlquad2,    FALSE,  TRUE,   D3DDP_DONOTLIGHT,   0x008080ff},
        { FVF_TLVERTEX2,    tlquad2,    TRUE,   TRUE,   D3DDP_DONOTLIGHT,   0x008080ff},
    };

    window = CreateWindowA("static", "ddraw_test", WS_OVERLAPPEDWINDOW,
            0, 0, 640, 480, 0, 0, 0, 0);

    if (!(device = create_device(window, DDSCL_NORMAL)))
    {
        skip("Failed to create D3D device, skipping test.\n");
        DestroyWindow(window);
        return;
    }

    hr = IDirect3DDevice7_GetRenderTarget(device, &rt);
    ok(SUCCEEDED(hr), "Failed to get render target, hr %#x.\n", hr);

    memset(&material, 0, sizeof(material));
    material.emissive.g = 1.0f;
    hr = IDirect3DDevice7_SetMaterial(device, &material);
    ok(SUCCEEDED(hr), "Failed set material, hr %#x.\n", hr);
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    ok(SUCCEEDED(hr), "Failed to disable z test, hr %#x.\n", hr);

    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_LIGHTING, &rs);
    ok(SUCCEEDED(hr), "Failed to get lighting render state, hr %#x.\n", hr);
    ok(rs == TRUE, "Initial D3DRENDERSTATE_LIGHTING is %#x, expected TRUE.\n", rs);
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_SPECULARENABLE, &rs);
    ok(SUCCEEDED(hr), "Failed to get specularenable render state, hr %#x.\n", hr);
    ok(rs == FALSE, "Initial D3DRENDERSTATE_SPECULARENABLE is %#x, expected FALSE.\n", rs);

    for (i = 0; i < sizeof(tests) / sizeof(*tests); i++)
    {
        hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff202020, 0.0f, 0);
        ok(SUCCEEDED(hr), "Failed to clear viewport, hr %#x.\n", hr);

        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, tests[i].d3drs_lighting);
        ok(SUCCEEDED(hr), "Failed to set lighting render state, hr %#x.\n", hr);
        hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_SPECULARENABLE,
                tests[i].d3drs_specular);
        ok(SUCCEEDED(hr), "Failed to set specularenable render state, hr %#x.\n", hr);

        hr = IDirect3DDevice7_BeginScene(device);
        ok(SUCCEEDED(hr), "Failed to begin scene, hr %#x.\n", hr);
        hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLESTRIP,
                tests[i].vertextype, tests[i].data, 4, tests[i].draw_flags | D3DDP_WAIT);
        hr = IDirect3DDevice7_EndScene(device);
        ok(SUCCEEDED(hr), "Failed to end scene, hr %#x.\n", hr);

        hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_LIGHTING, &rs);
        ok(SUCCEEDED(hr), "Failed to get lighting render state, hr %#x.\n", hr);
        ok(rs == tests[i].d3drs_lighting, "D3DRENDERSTATE_LIGHTING is %#x, expected %#x.\n",
                rs, tests[i].d3drs_lighting);

        color = get_surface_color(rt, 320, 240);
        ok(compare_color(color, tests[i].color, 1),
                "Got unexpected color 0x%08x, expected 0x%08x, test %u.\n",
                color, tests[i].color, i);
    }

    IDirectDrawSurface7_Release(rt);
    ref = IDirect3DDevice7_Release(device);
    ok(ref == 0, "Device not properly released, refcount %u.\n", ref);
}

START_TEST(ddraw7)
{
    HMODULE module = GetModuleHandleA("ddraw.dll");

    if (!(pDirectDrawCreateEx = (void *)GetProcAddress(module, "DirectDrawCreateEx")))
    {
        win_skip("DirectDrawCreateEx not available, skipping tests.\n");
        return;
    }

    test_process_vertices();
    test_coop_level_create_device_window();
    test_clipper_blt();
    test_coop_level_d3d_state();
    test_surface_interface_mismatch();
    test_coop_level_threaded();
    test_depth_blit();
    test_texture_load_ckey();
    test_zenable();
    test_ck_rgba();
    test_ck_default();
    test_surface_qi();
    test_device_qi();
    test_wndproc();
    test_window_style();
    test_redundant_mode_set();
    test_coop_level_mode_set();
    test_coop_level_mode_set_multi();
    test_initialize();
    test_coop_level_surf_create();
    test_vb_discard();
    test_coop_level_multi_window();
    test_draw_strided();
    test_clear_rect_count();
    test_coop_level_versions();
    test_fog_special();
    test_lighting_interface_versions();
}
