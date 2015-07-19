/*
** settingsmenu.cpp
**
** This file is part of mkxp.
**
** Copyright (C) 2014 Jonas Kulla <Nyocurio@gmail.com>
**
** mkxp is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 2 of the License, or
** (at your option) any later version.
**
** mkxp is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with mkxp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "settingsmenu.h"

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_keyboard.h>

#include "keybindings.h"
#include "eventthread.h"
#include "font.h"
#include "input.h"
#include "etc-internal.h"
#include "util.h"
#include "gl-fun.h"
#include "bundledfont.h"
#include "eventthread.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"

#include <algorithm>
#include <assert.h>

const Vec2i winSize(740, 400);

const float fontSize = 16.0f;

const ImVec4 colButton = ImColor(96,96,96);
const ImVec4 colButtonHover = ImColor(51,51,51);
const ImVec4 colBackground = ImColor(128,128,128);

const uint8_t numCols = 3;
const uint8_t numRows = 4;

typedef SettingsMenuPrivate SMP;

#define BTN_STRING(btn,desc) { Input:: btn, #desc }
struct VButton
{
	Input::ButtonCode code;
	const char *str;
} static const vButtons[] =
{
	BTN_STRING(Up,Up),
	BTN_STRING(Down,Down),
	BTN_STRING(L,L),
	BTN_STRING(Left,Left),
	BTN_STRING(Right,Right),
	BTN_STRING(R,W-Atk),
	BTN_STRING(A,Dismount),
	BTN_STRING(B,Cancel),
	BTN_STRING(C,Confirm),
	BTN_STRING(X,A-Atk),
	BTN_STRING(Y,S-Atk),
	BTN_STRING(Z,D-Atk)
};

static elementsN(vButtons);

/* Macros to read/write central config and check for changed values */
#define STORE_CONFIG(key) rtData.config. key = tempConfig. key; rtData.config.store(#key, rtData.config. key )
#define VALUE_CHANGED(key) (rtData.config. key != tempConfig. key)

/* Holds configurables that can be modified in the settings menu
/* until they get all written out the config file and applied */
struct Configurables
{
	bool fullscreen;
	bool fixedAspectRatio;
	bool smoothScaling;
	bool vsync;
	int defScreenW;
	int defScreenH;
	bool frameSkip;
	bool solidFonts;

	Configurables()
	{
	}

	Configurables(Config &c)
	{
		fullscreen = c.fullscreen;
		fixedAspectRatio = c.fixedAspectRatio;
		smoothScaling = c.smoothScaling;
		vsync = c.vsync;
		defScreenW = c.defScreenW;
		defScreenH = c.defScreenH;
		frameSkip = c.frameSkip;
		solidFonts = c.solidFonts;
	}

} static tempConfig;

/* Human readable string representation */
std::string sourceDescString(const SourceDesc &src)
{
	char buf[128];
	char pos;

	switch (src.type)
	{
	case Invalid:
		return std::string();

	case Key:
	{
		if (src.d.scan == SDL_SCANCODE_LSHIFT)
			return "Shift";

		SDL_Keycode key = SDL_GetKeyFromScancode(src.d.scan);
		const char *str = SDL_GetKeyName(key);

		if (*str == '\0')
			return "Unknown key";
		else
			return str;
	}
	case JButton:
		snprintf(buf, sizeof(buf), "JS %d", src.d.jb);
		return buf;

	case JHat:
		switch(src.d.jh.pos)
		{
		case SDL_HAT_UP:
			pos = 'U';
			break;

		case SDL_HAT_DOWN:
			pos = 'D';
			break;

		case SDL_HAT_LEFT:
			pos = 'L';
			break;

		case SDL_HAT_RIGHT:
			pos = 'R';
			break;

		default:
			pos = '-';
		}
		snprintf(buf, sizeof(buf), "Hat %d:%c",
		         src.d.jh.hat, pos);
		return buf;

	case JAxis:
		snprintf(buf, sizeof(buf), "Axis %d%c",
		         src.d.ja.axis, src.d.ja.dir == Negative ? '-' : '+');
		return buf;
	}

	assert(!"unreachable");
	return "";
}

struct BindingWidget
{
	SMP *p;
	VButton vb;
	/* Source slots */
	SourceDesc src[4];
	/* Flag indicating whether a slot source is used
	 * for multiple button targets (red indicator) */
	bool dupFlag[4];

	BindingWidget(int vbIndex, SMP *p)
	    : vb(vButtons[vbIndex]), p(p)
	{}

	void appendBindings(BDescVec &d) const;
	void click(SourceDesc& desc);
	void displayWidget(uint32_t width, uint32_t height);
};

enum State
{
	Idle,
	AwaitingInput
};

static bool resCheckbox[3];

struct SettingsMenuPrivate
{
	State state;

	/* Necessary to decide which window gets to
	 * process joystick events */
	bool hasFocus;

	/* Tell the outer EventThread to destroy us */
	bool destroyReq;

	/* Set to true if there are any duplicate bindings */
	bool dupWarn;

	SDL_Window *window;
	SDL_GLContext glContext;
	uint32_t winID;

	enum tabs
	{
		CONTROLS,
		GRAPHICS
	};

	enum tabs currentTab;

	RGSSThreadData &rtData;

	std::vector<BindingWidget> bWidgets;

	SourceDesc *captureDesc;
	const char *captureName;

	SettingsMenuPrivate(RGSSThreadData &rtData)
	    : rtData(rtData)
	{
	}

	void setupBindingData(const BDescVec &d)
	{
		size_t slotI[vButtonsN] = { 0 };

		for (size_t i = 0; i < bWidgets.size(); ++i)
			for (size_t j = 0; j < 4; ++j)
				bWidgets[i].src[j].type = Invalid;

		for (size_t i = 0; i < d.size(); ++i)
		{
			const BindingDesc &desc = d[i];
			const Input::ButtonCode trg = desc.target;

			size_t j;
			for (j = 0; j < vButtonsN; ++j)
				if (bWidgets[j].vb.code == trg)
					break;

			assert(j < vButtonsN);

			size_t &slot = slotI[j];
			BindingWidget &w = bWidgets[j];

			if (slot == 4)
				continue;

			w.src[slot++] = desc.src;
		}
	}

	void updateDuplicateStatus()
	{
		for (size_t i = 0; i < bWidgets.size(); ++i)
			for (size_t j = 0; j < 4; ++j)
				bWidgets[i].dupFlag[j] = false;

		dupWarn = false;

		for (size_t i = 0; i < bWidgets.size(); ++i)
		{
			for (size_t j = 0; j < 4; ++j)
			{
				const SourceDesc &src = bWidgets[i].src[j];

				if (src.type == Invalid)
					continue;

				for (size_t k = 0; k < bWidgets.size(); ++k)
				{
					if (k == i)
						continue;

					for (size_t l = 0; l < 4; ++l)
					{
						if (bWidgets[k].src[l] != src)
							continue;

						bWidgets[i].dupFlag[j] = true;
						bWidgets[k].dupFlag[l] = true;
						dupWarn = true;
					}
				}
			}
		}
	}

	void redraw()
	{
		ImGui_ImplSdl_NewFrame(window);
		{
			ImGui::SetNextWindowSize(ImVec2((float)winSize.x,(float)winSize.y));
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			
			ImGuiWindowFlags WindowFlags = 0;
			WindowFlags |= ImGuiWindowFlags_NoTitleBar;
			WindowFlags |= ImGuiWindowFlags_NoResize;
			WindowFlags |= ImGuiWindowFlags_NoMove;
			WindowFlags |= ImGuiWindowFlags_NoScrollbar;
			WindowFlags |= ImGuiWindowFlags_NoCollapse;
			WindowFlags |= ImGuiWindowFlags_NoScrollWithMouse;
			WindowFlags |= ImGuiWindowFlags_NoSavedSettings;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

			ImGui::PushStyleColor(ImGuiCol_Button, colButton);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colButtonHover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, colButtonHover);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, colBackground);

			bool bTrue = true;
			ImGui::Begin("Container", &bTrue, WindowFlags);

			tabSelector("Controls", CONTROLS);
			ImGui::SameLine();
			ImGui::Text("|");
			ImGui::SameLine();
			if(tabSelector("Graphics", GRAPHICS))
			{
				tempConfig = Configurables(rtData.config);
			}

			ImGui::Separator();

			switch(currentTab)
			{
				case CONTROLS:
					displayControllerTab();
					break;

				case GRAPHICS:
					displayGraphicsTab();
					break;
			}
			ImGui::End();
			ImGui::PopStyleColor(4);
			ImGui::PopStyleVar();
		}
		ImGui::Render();
		SDL_GL_SwapWindow(window);
	}

	bool tabSelector(const char * tabName, tabs tabId)
	{
		if (ImGui::Selectable(tabName, currentTab == tabId, 0, ImGui::CalcTextSize(tabName)) && (state == Idle))
		{
			currentTab = tabId;
			return true;
		}
		return false;
	}

	void displayControllerTab()
	{
		ImVec4 red = ImColor(255, 0, 0);
		if(state == AwaitingInput)
		{
			ImGui::Dummy(ImVec2(0, ImGui::GetWindowContentRegionMax().y/2 - fontSize/2));
			ImGui::Text("Press key or joystick button for \"%s\"", captureName);
		}
		else
		{
			/* Header Text */
			ImGui::Text("Use left click to bind a slot, right click to clear its binding");
			if(dupWarn)
			{
				ImGui::TextColored(red, "Warning: Same physical key bound to multiple slots");
			}

			/* Button Assignment Widgets */
			uint32_t widgetWidth = (ImGui::GetWindowContentRegionMax().x-ImGui::GetStyle().WindowPadding.x) / numCols;
			uint32_t widgetHeight = 64;
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
			ImGui::PushStyleColor(ImGuiCol_ChildWindowBg, ImColor(0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Button, colBackground);
			ImGui::BeginChild("Table", ImVec2(numCols*widgetWidth+8, numRows*widgetHeight+2));
			ImGui::Spacing();

			int i = 0;
			for(int y = 0; y < numRows; y++)
			{
				ImGui::Dummy(ImVec2(0, 0));
				for(int x = 0; x < numCols; x++)
				{
					ImGui::SameLine();
					bWidgets[i].displayWidget(widgetWidth, widgetHeight); 
					i++;
				}
			}

			ImGui::EndChild();
			ImGui::PopStyleColor(2);
			ImGui::PopStyleVar();

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			/* Bottom Buttons */
			ImVec2 btnDim = ImVec2(100, 24);
			if(ImGui::Button("Reset Default", btnDim))
			{
				onResetToDefault();
			}
			ImGui::SameLine();
			ImGui::Dummy(ImVec2(ImGui::GetWindowContentRegionMax().x - ImGui::GetStyle().WindowPadding.x - 3*btnDim.x - 2*ImGui::GetStyle().ItemSpacing.x, btnDim.y));
			ImGui::SameLine();

			if(ImGui::Button("Cancel", btnDim))
			{
				onCancel();
			}
			ImGui::SameLine();

			if(ImGui::Button("Store", btnDim))
			{
				onAccept();
			}
		}
	}

	static inline bool resolutionEqualsN(int* x, int* y, int n)
	{
		return ((x[0] == n*y[0]) && (x[1] == n*y[1]));
	}

	static inline bool TextCheckbox(const char* str_id, bool active, bool &hovered, const ImVec2 &size)
	{
		bool result;
		bool pushedFour = false;
		ImVec2 innerPadding = ImGui::GetStyle().FramePadding;
		ImVec2 innerSize = ImVec2(size.x-2*innerPadding.x, size.y-2*innerPadding.y);
		ImGuiID id = ImGui::GetID(str_id);

		/* Set button colors to match checkbox */
		if(active)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_CheckMark]);
		else
			ImGui::PushStyleColor(ImGuiCol_Button, ImColor(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_Button]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_CheckMark]);

		/* Was item hovered in the previous frame? */
		if(hovered)
		{
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
			pushedFour = true;
		}
		ImGui::BeginChildFrame(id, size);
		hovered = ImGui::IsWindowHovered();

		/* Align button properly in child window */
		ImVec2 pos = ImGui::GetWindowPos();
		pos.x += innerPadding.x;
		pos.y += innerPadding.y;
		ImGui::SetWindowPos(pos);
		result = ImGui::Button(str_id, innerSize);
		ImGui::EndChildFrame();
		ImGui::PopStyleColor(pushedFour ? 4 : 3);
		return result;
	}

	static inline void TextCentered(const char* str_id, const ImVec2 &size)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
		ImGui::Button(str_id, size);
		ImGui::PopStyleColor(3);
	}

	void displayGraphicsTab()
	{
		if(ImGui::CollapsingHeader("Display Settings", 0, true, true))
		{
			/* current resolution and native rendering resolution */
			int *res = &tempConfig.defScreenW;
			int native[2] = {(rtData.config.rgssVersion == 1 ? 640 : 544),
							 (rtData.config.rgssVersion == 1 ? 480 : 416)};

			if(ImGui::InputInt2("Window Size", res))
			{
				/* clamp to between 320x240 and 4K resolutions */
				tempConfig.defScreenW = std::min(std::max(tempConfig.defScreenW, 320),4096);
				tempConfig.defScreenH = std::min(std::max(tempConfig.defScreenH, 240),2160);
			}
			if(TextCheckbox("1X native", resolutionEqualsN(res, native, 1), resCheckbox[0], ImVec2(80, 24)))
			{
				res[0] = native[0];
				res[1] = native[1];
			}
			ImGui::SameLine();
			if(TextCheckbox("2X native", resolutionEqualsN(res, native, 2), resCheckbox[1], ImVec2(80, 24)))
			{
				res[0] = 2*native[0];
				res[1] = 2*native[1];
			}
			ImGui::SameLine();
			if(TextCheckbox("3X native", resolutionEqualsN(res, native, 3), resCheckbox[2], ImVec2(80, 24)))
			{
				res[0] = 3*native[0];
				res[1] = 3*native[1];
			}
			ImGui::SameLine();
			TextCentered("Recommended if no smooth upscaling.", ImVec2(0, 24));
			ImGui::Checkbox("Start in fullscreen", &tempConfig.fullscreen);
			ImGui::SameLine();
			ImGui::Checkbox("Keep aspect ratio", &tempConfig.fixedAspectRatio);
		}

		ImGui::Dummy(ImVec2(0, 48));

		if(ImGui::CollapsingHeader("Quality Settings", 0, true, true))
		{
			ImGui::Checkbox("Enable smooth upscaling", &tempConfig.smoothScaling);
			ImGui::Checkbox("Enable vertical sync", &tempConfig.vsync);
			ImGui::Checkbox("Skip frames when too slow", &tempConfig.frameSkip);
			ImGui::Checkbox("Fast font rendering", &tempConfig.solidFonts);
		}

		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		/* Buttons */
		ImVec2 btnDim = ImVec2(150, 24);
		ImGui::Dummy(ImVec2(ImGui::GetWindowContentRegionMax().x - ImGui::GetStyle().WindowPadding.x - 2*btnDim.x - 1*ImGui::GetStyle().ItemSpacing.x, btnDim.y));
		ImGui::SameLine();
		if(ImGui::Button("Discard Changes", btnDim))
		{
			tempConfig = Configurables(rtData.config);
		}
		ImGui::SameLine();
		if(ImGui::Button("Apply Changes", btnDim))
		{
			bool refreshWindow = false;
			if(VALUE_CHANGED(defScreenW) || VALUE_CHANGED(defScreenH))
			{
				STORE_CONFIG(defScreenW);
				STORE_CONFIG(defScreenH);
				refreshWindow = true;
			}

			if(VALUE_CHANGED(fullscreen))
			{
				STORE_CONFIG(fullscreen);
			}

			if(VALUE_CHANGED(fixedAspectRatio))
			{
				STORE_CONFIG(fixedAspectRatio);
				refreshWindow = true;
			}

			if(VALUE_CHANGED(smoothScaling))
			{
				STORE_CONFIG(smoothScaling);
			}

			if(VALUE_CHANGED(vsync))
			{
				STORE_CONFIG(vsync);
			}

			if(VALUE_CHANGED(frameSkip))
			{
				STORE_CONFIG(frameSkip);
			}

			if(VALUE_CHANGED(solidFonts))
			{
				STORE_CONFIG(solidFonts);
			}

			if(refreshWindow)
				SDL_SetWindowSize(rtData.window, tempConfig.defScreenW, tempConfig.defScreenH);
		}
	}

	bool onCaptureInputEvent(const SDL_Event &event)
	{
		assert(captureDesc);
		SourceDesc &desc = *captureDesc;

		switch (event.type)
		{
		case SDL_KEYDOWN:
			desc.type = Key;
			desc.d.scan = event.key.keysym.scancode;

			/* Special case aliases */
			if (desc.d.scan == SDL_SCANCODE_RSHIFT)
				desc.d.scan = SDL_SCANCODE_LSHIFT;

			if (desc.d.scan == SDL_SCANCODE_KP_ENTER)
				desc.d.scan = SDL_SCANCODE_RETURN;

			break;

		case SDL_JOYBUTTONDOWN:
			desc.type = JButton;
			desc.d.jb = event.jbutton.button;
			break;

		case SDL_JOYHATMOTION:
		{
			int v = event.jhat.value;

			/* Only register if single directional input */
			if (v != SDL_HAT_LEFT && v != SDL_HAT_RIGHT &&
			    v != SDL_HAT_UP   && v != SDL_HAT_DOWN)
				return true;

			desc.type = JHat;
			desc.d.jh.hat = event.jhat.hat;
			desc.d.jh.pos = v;
			break;
		}

		case SDL_JOYAXISMOTION:
		{
			int v = event.jaxis.value;

			/* Only register if pushed halfway through */
			if (v > -JAXIS_THRESHOLD && v < JAXIS_THRESHOLD)
				return true;

			desc.type = JAxis;
			desc.d.ja.axis = event.jaxis.axis;
			desc.d.ja.dir = v < 0 ? Negative : Positive;
			break;
		}
		default:
			return false;
		}

		captureDesc = 0;
		state = Idle;
		updateDuplicateStatus();

		return true;
	}

	void onResetToDefault()
	{
		setupBindingData(genDefaultBindings(rtData.config, rtData.gamecontroller));
		updateDuplicateStatus();
	}

	void onAccept()
	{
		BDescVec binds;

		for (size_t i = 0; i < bWidgets.size(); ++i)
			bWidgets[i].appendBindings(binds);

		rtData.bindingUpdateMsg.post(binds);

		/* Store the key bindings to disk as well to prevent config loss */
		storeBindings(binds, rtData.config);
	}

	void onCancel()
	{
		destroyReq = true;
	}
};

void BindingWidget::click(SourceDesc &desc)
{
	/* Check for right click */
	if(ImGui::IsMouseClicked(1))
	{
		desc.type = Invalid;
		p->updateDuplicateStatus();
		return;
	}

	p->captureDesc = &desc;
	p->captureName = vb.str;
	p->state = AwaitingInput;
}
	
void BindingWidget::displayWidget(uint32_t width, uint32_t height)
{
	ImVec2 buttonSize = ImVec2((width-6)/3, height/2-ImGui::GetStyle().ItemSpacing.x);
	ImGui::PushID(vb.code);

	/* Label for Widget */
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_WindowBg]);
	ImGui::Button(vb.str, ImVec2(width/3, height-ImGui::GetStyle().ItemSpacing.x));
	ImGui::PopStyleColor();
	ImGui::SameLine();

	/* Group of buttons */
	ImGui::BeginGroup();
	for(int i=0; i<4; i++)
	{
		if(dupFlag[i])
			ImGui::PushStyleColor(ImGuiCol_Text, ImColor(255, 0, 0));

		if(ImGui::Button(sourceDescString(src[i]).c_str(), buttonSize))
			click(src[i]);

		if(dupFlag[i])
			ImGui::PopStyleColor();

		if(i%2 == 0)
			ImGui::SameLine();
	}
	ImGui::EndGroup();

	ImGui::PopID();
}

void BindingWidget::appendBindings(BDescVec &d) const
{
	for (size_t i = 0; i < 4; ++i)
	{
		if (src[i].type == Invalid)
			continue;

		BindingDesc desc;
		desc.src = src[i];
		desc.target = vb.code;
		d.push_back(desc);
	}
}

SettingsMenu::SettingsMenu(RGSSThreadData &rtData)
{
	p = new SettingsMenuPrivate(rtData);
	p->state = Idle;

	p->hasFocus = false;
	p->destroyReq = false;
	p->dupWarn = false;

	p->currentTab = SettingsMenuPrivate::CONTROLS;

	p->window = SDL_CreateWindow("Settings Menu",
	                             SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                             winSize.x, winSize.y, SDL_WINDOW_OPENGL|SDL_WINDOW_INPUT_FOCUS);
	p->winID = SDL_GetWindowID(p->window);
	p->glContext = SDL_GL_CreateContext(p->window);

	ImGui_ImplSdl_Init(p->window);

	/* ImGUI wants to own the memory with the TTF data, so requires a copy. */
	void * liberation_copy = malloc(BNDL_F_L(BUNDLED_FONT));
	memcpy(liberation_copy, BNDL_F_D(BUNDLED_FONT), BNDL_F_L(BUNDLED_FONT));
	ImGuiIO& io = ImGui::GetIO();
	ImFont* im_font = io.Fonts->AddFontFromMemoryTTF(liberation_copy, BNDL_F_L(BUNDLED_FONT), 16.0f);

	/* Generate Binding Widgets */
	assert(numRows*numCols == vButtonsN);

	for (int i = 0; i < vButtonsN; i++)
	{
		BindingWidget w(i, p);
		p->bWidgets.push_back(w);
	}

	BDescVec binds;
	rtData.bindingUpdateMsg.get(binds);
	p->setupBindingData(binds);

	p->captureDesc = 0;
	p->captureName = 0;

	p->updateDuplicateStatus();

	p->redraw();
}

SettingsMenu::~SettingsMenu()
{
	ImGui_ImplSdl_Shutdown();
	SDL_GL_DeleteContext(p->glContext);
	SDL_DestroyWindow(p->window);

	delete p;
}

bool SettingsMenu::onEvent(const SDL_Event &event)
{
	/* Check for a redraw event first */
	if(event.type == EventThread::UPDATE_POPUP + EventThread::UsrIdStart)
	{
		if(p->hasFocus)
			p->redraw();
	}

	/* Check whether this event is for us */
	switch (event.type)
	{
	case SDL_WINDOWEVENT :
	case SDL_MOUSEBUTTONDOWN :
	case SDL_MOUSEBUTTONUP :
	case SDL_MOUSEMOTION :
	case SDL_KEYDOWN :
	case SDL_KEYUP :
	case SDL_TEXTINPUT :
		/* We can do this because windowID has the same
		 * struct offset in all these event types */
		if (event.window.windowID != p->winID)
			return false;
		break;

	case SDL_JOYBUTTONDOWN :
	case SDL_JOYBUTTONUP :
	case SDL_JOYHATMOTION :
	case SDL_JOYAXISMOTION :
		if (!p->hasFocus)
			return false;
		break;

	/* Don't try to handle something we don't understand */
	default:
		return false;
	}

	/* Pass through event data to ImGUI */
	ImGui_ImplSdl_ProcessEvent(event);

	/* Now process it.. */
	switch (event.type)
	{
	/* Ignore these event */
	case SDL_MOUSEBUTTONUP :
	case SDL_KEYUP :
		return true;

	case SDL_WINDOWEVENT :
		switch (event.window.event)
		{
		case SDL_WINDOWEVENT_SHOWN : // SDL is bugged and doesn't give us a first FOCUS_GAINED event
		case SDL_WINDOWEVENT_FOCUS_GAINED :
			p->hasFocus = true;
			break;

		case SDL_WINDOWEVENT_FOCUS_LOST :
			p->hasFocus = false;
			break;

		case SDL_WINDOWEVENT_EXPOSED :
			p->redraw();
			break;

		case SDL_WINDOWEVENT_CLOSE:
			p->onCancel();
		}

		return true;

	case SDL_MOUSEMOTION:
		//p->onMotion(event.motion);
		return true;

	case SDL_KEYDOWN:
		if (p->state != AwaitingInput)
		{
			if (event.key.keysym.sym == SDLK_RETURN)
				p->onAccept();
			else if (event.key.keysym.sym == SDLK_ESCAPE)
				p->onCancel();

			return true;
		}

		/* Don't let the user bind keys that trigger
		 * mkxp functions */
		switch(event.key.keysym.scancode)
		{
		case SDL_SCANCODE_F1:
		case SDL_SCANCODE_F2:
		case SDL_SCANCODE_F12:
			return true;
		default:
			break;
		}

	case SDL_JOYBUTTONDOWN:
	case SDL_JOYHATMOTION:
	case SDL_JOYAXISMOTION:
		if (p->state != AwaitingInput)
			return true;
		break;

	case SDL_MOUSEBUTTONDOWN:
		return true;

	default:
		return true;
	}

	if (p->state == AwaitingInput)
		return p->onCaptureInputEvent(event);

	return true;
}

void SettingsMenu::raise()
{
	SDL_RaiseWindow(p->window);
}

bool SettingsMenu::destroyReq() const
{
	return p->destroyReq;
}
