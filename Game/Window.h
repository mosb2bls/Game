#pragma once

#define NOMINMAX
#include <Windows.h>
#include <string>

#define WINDOW_GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define WINDOW_GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))

class Window
{
public:
	HWND hwnd;
	HINSTANCE hinstance;
	int width;
	int height;
	float invZoom;
	std::string name;
	bool keys[256];
	int mousex;
	int mousey;
	bool mouseButtons[3];
	int mouseWheel;
	bool useMouseClip;
	void updateMouse(int x, int y)
	{
		mousex = x;
		mousey = y;
	}
	void processMessages();
	void create(int window_width, int window_height, const std::string window_name, float zoom = 1.0f, bool window_fullscreen = false, int window_x = 0, int window_y = 0);
	void checkInput()
	{
		if (useMouseClip)
		{
			clipMouseToWindow();
		}
		processMessages();
	}
	bool keyPressed(int key)
	{
		return keys[key];
	}
	int getMouseInWindowX()
	{
		POINT p;
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		RECT rect;
		GetClientRect(hwnd, &rect);
		p.x = p.x - rect.left;
		p.x = p.x * invZoom;
		return p.x;
	}
	int getMouseInWindowY()
	{
		POINT p;
		GetCursorPos(&p);
		ScreenToClient(hwnd, &p);
		RECT rect;
		GetClientRect(hwnd, &rect);
		p.y = p.y - rect.top;
		p.y = p.y * invZoom;
		return p.y;
	}
	void clipMouseToWindow()
	{
		RECT rect;
		GetClientRect(hwnd, &rect);
		POINT ul;
		ul.x = rect.left;
		ul.y = rect.top;
		POINT lr;
		lr.x = rect.right;
		lr.y = rect.bottom;
		MapWindowPoints(hwnd, nullptr, &ul, 1);
		MapWindowPoints(hwnd, nullptr, &lr, 1);
		rect.left = ul.x;
		rect.top = ul.y;
		rect.right = lr.x;
		rect.bottom = lr.y;
		ClipCursor(&rect);
	}
	~Window()
	{
		ShowCursor(true);
		ClipCursor(NULL);
	}
};