// ImGui SDL2 binding with OpenGL
// https://github.com/ocornut/imgui

#include <SDL.h>
#include <SDL_syswm.h>
#include "gl-fun.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"

// Data
static double       g_Time = 0.0f;
static bool         g_MousePressed[3] = { false, false, false };
static float        g_MouseWheel = 0.0f;
static GLuint       g_FontTexture = 0;
static int          g_ShaderHandle = 0, g_VertHandle = 0, g_FragHandle = 0;
static int          g_AttribLocationTex = 0, g_AttribLocationProjMtx = 0;
static int          g_AttribLocationPosition = 0, g_AttribLocationUV = 0, g_AttribLocationColor = 0;
static int          g_VboSize = 0;
static unsigned int g_VboHandle = 0, g_VaoHandle = 0;

// This is the main rendering function that you have to implement and provide to ImGui (via setting up 'RenderDrawListsFn' in the ImGuiIO structure)
// If text or lines are blurry when integrating ImGui in your engine:
// - in your Render function, try translating your projection matrix by (0.5f,0.5f) or (0.375f,0.375f)
static void ImGui_ImplSdl_RenderDrawLists(ImDrawList** const cmd_lists, int cmd_lists_count)
{
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled
    GLint last_program, last_texture;
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &last_program);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    gl.Enable(GL_BLEND);
    gl.BlendEquation(GL_FUNC_ADD);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.Disable(GL_CULL_FACE);
    gl.Disable(GL_DEPTH_TEST);
    gl.Enable(GL_SCISSOR_TEST);
    gl.ActiveTexture(GL_TEXTURE0);

    // Setup orthographic projection matrix
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
    const float ortho_projection[4][4] =
    {
        { 2.0f/width,   0.0f,           0.0f,       0.0f },
        { 0.0f,         2.0f/-height,   0.0f,       0.0f },
        { 0.0f,         0.0f,           -1.0f,      0.0f },
        { -1.0f,        1.0f,           0.0f,       1.0f },
    };
    gl.UseProgram(g_ShaderHandle);
    gl.Uniform1i(g_AttribLocationTex, 0);
    gl.UniformMatrix4fv(g_AttribLocationProjMtx, 1, GL_FALSE, &ortho_projection[0][0]);

    // Grow our buffer according to what we need
    int total_vtx_count = 0;
    for (int n = 0; n < cmd_lists_count; n++)
        total_vtx_count += cmd_lists[n]->vtx_buffer.size();
    gl.BindBuffer(GL_ARRAY_BUFFER, g_VboHandle);
    int needed_vtx_size = total_vtx_count * sizeof(ImDrawVert);
    if (g_VboSize < needed_vtx_size)
    {
        g_VboSize = needed_vtx_size + 5000 * sizeof(ImDrawVert);  // Grow buffer
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_VboSize, NULL, GL_STREAM_DRAW);
    }

    // Copy and convert all vertices into a single contiguous buffer
    unsigned char* buffer_data = (unsigned char*)gl.MapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    if (!buffer_data)
        return;
    for (int n = 0; n < cmd_lists_count; n++)
    {
        const ImDrawList* cmd_list = cmd_lists[n];
        memcpy(buffer_data, &cmd_list->vtx_buffer[0], cmd_list->vtx_buffer.size() * sizeof(ImDrawVert));
        buffer_data += cmd_list->vtx_buffer.size() * sizeof(ImDrawVert);
    }
    gl.UnmapBuffer(GL_ARRAY_BUFFER);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);
    gl.BindVertexArray(g_VaoHandle);

    int cmd_offset = 0;
    for (int n = 0; n < cmd_lists_count; n++)
    {
        const ImDrawList* cmd_list = cmd_lists[n];
        int vtx_offset = cmd_offset;
        const ImDrawCmd* pcmd_end = cmd_list->commands.end();
        for (const ImDrawCmd* pcmd = cmd_list->commands.begin(); pcmd != pcmd_end; pcmd++)
        {
            if (pcmd->user_callback)
            {
                pcmd->user_callback(cmd_list, pcmd);
            }
            else
            {
                gl.BindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->texture_id);
                gl.Scissor((int)pcmd->clip_rect.x, (int)(height - pcmd->clip_rect.w), (int)(pcmd->clip_rect.z - pcmd->clip_rect.x), (int)(pcmd->clip_rect.w - pcmd->clip_rect.y));
                gl.DrawArrays(GL_TRIANGLES, vtx_offset, pcmd->vtx_count);
            }
            vtx_offset += pcmd->vtx_count;
        }
        cmd_offset = vtx_offset;
    }

    // Restore modified state
    gl.BindVertexArray(0);
    gl.UseProgram(last_program);
    gl.Disable(GL_SCISSOR_TEST);
    gl.BindTexture(GL_TEXTURE_2D, last_texture);
}

static const char* ImGui_ImplSdl_GetClipboardText()
{
	return SDL_GetClipboardText();
}

static void ImGui_ImplSdl_SetClipboardText(const char* text)
{
    SDL_SetClipboardText(text);
}

bool ImGui_ImplSdl_ProcessEvent(const SDL_Event &event)
{
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type)
    {
    case SDL_MOUSEWHEEL:
        {
            if (event.wheel.y > 0)
                g_MouseWheel = 1;
            if (event.wheel.y < 0)
                g_MouseWheel = -1;
            return true;
        }
    case SDL_MOUSEBUTTONDOWN:
        {
            if (event.button.button == SDL_BUTTON_LEFT) g_MousePressed[0] = true;
            if (event.button.button == SDL_BUTTON_RIGHT) g_MousePressed[1] = true;
            if (event.button.button == SDL_BUTTON_MIDDLE) g_MousePressed[2] = true;
            return true;
        }
    case SDL_TEXTINPUT:
        {
            ImGuiIO& io = ImGui::GetIO();
            const char * text = event.text.text;
            for(int i=0; i<16; i++)
            {
                char key = text[i];
                if(key>31 && key<256)
                {
                    io.AddInputCharacter(key);
                }
                
                if(key == '\0')
                {
                    break;
                }
            }
            return true;
        }
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        {
            int key = SDL_GetScancodeFromKey(event.key.keysym.sym);
            io.KeysDown[key] = (event.type == SDL_KEYDOWN);
            io.KeyShift = ((SDL_GetModState() & KMOD_SHIFT) != 0);
            io.KeyCtrl = ((SDL_GetModState() & KMOD_CTRL) != 0);
            io.KeyAlt = ((SDL_GetModState() & KMOD_ALT) != 0);
            return true;
        }
    }
    return false;
}

void ImGui_ImplSdl_CreateFontsTexture()
{
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);   // Load as RGBA 32-bits for OpenGL3 demo because it is more likely to be compatible with user's existing shader.

    gl.GenTextures(1, &g_FontTexture);
    gl.BindTexture(GL_TEXTURE_2D, g_FontTexture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Store our identifier
    io.Fonts->TexID = (void *)(intptr_t)g_FontTexture;

    // Cleanup (don't clear the input data if you want to append new fonts later)
    io.Fonts->ClearInputData();
    io.Fonts->ClearTexData();
}

bool ImGui_ImplSdl_CreateDeviceObjects()
{
    const GLchar *vertex_shader =
        "#version 130\n"
        "uniform mat4 ProjMtx;\n"
        "in vec2 Position;\n"
        "in vec2 UV;\n"
        "in vec4 Color;\n"
        "out vec2 Frag_UV;\n"
        "out vec4 Frag_Color;\n"
        "void main()\n"
        "{\n"
        "   Frag_UV = UV;\n"
        "   Frag_Color = Color;\n"
        "   gl_Position = ProjMtx * vec4(Position.xy,0,1);\n"
        "}\n";

    const GLchar* fragment_shader =
        "#version 130\n"
        "uniform sampler2D Texture;\n"
        "in vec2 Frag_UV;\n"
        "in vec4 Frag_Color;\n"
        "out vec4 Out_Color;\n"
        "void main()\n"
        "{\n"
        "   Out_Color = Frag_Color * texture( Texture, Frag_UV.st);\n"
        "}\n";

    g_ShaderHandle = gl.CreateProgram();
    g_VertHandle = gl.CreateShader(GL_VERTEX_SHADER);
    g_FragHandle = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(g_VertHandle, 1, &vertex_shader, 0);
    gl.ShaderSource(g_FragHandle, 1, &fragment_shader, 0);
    gl.CompileShader(g_VertHandle);
    gl.CompileShader(g_FragHandle);
    gl.AttachShader(g_ShaderHandle, g_VertHandle);
    gl.AttachShader(g_ShaderHandle, g_FragHandle);
    gl.LinkProgram(g_ShaderHandle);

    g_AttribLocationTex = gl.GetUniformLocation(g_ShaderHandle, "Texture");
    g_AttribLocationProjMtx = gl.GetUniformLocation(g_ShaderHandle, "ProjMtx");
    g_AttribLocationPosition = gl.GetAttribLocation(g_ShaderHandle, "Position");
    g_AttribLocationUV = gl.GetAttribLocation(g_ShaderHandle, "UV");
    g_AttribLocationColor = gl.GetAttribLocation(g_ShaderHandle, "Color");

    gl.GenBuffers(1, &g_VboHandle);

    gl.GenVertexArrays(1, &g_VaoHandle);
    gl.BindVertexArray(g_VaoHandle);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_VboHandle);
    gl.EnableVertexAttribArray(g_AttribLocationPosition);
    gl.EnableVertexAttribArray(g_AttribLocationUV);
    gl.EnableVertexAttribArray(g_AttribLocationColor);

#define OFFSETOF(TYPE, ELEMENT) ((size_t)&(((TYPE *)0)->ELEMENT))
    gl.VertexAttribPointer(g_AttribLocationPosition, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid*)OFFSETOF(ImDrawVert, pos));
    gl.VertexAttribPointer(g_AttribLocationUV, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), (GLvoid*)OFFSETOF(ImDrawVert, uv));
    gl.VertexAttribPointer(g_AttribLocationColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), (GLvoid*)OFFSETOF(ImDrawVert, col));
#undef OFFSETOF
    gl.BindVertexArray(0);
    gl.BindBuffer(GL_ARRAY_BUFFER, 0);

    ImGui_ImplSdl_CreateFontsTexture();

    return true;
}

bool ImGui_ImplSdl_Init(SDL_Window *window)
{
    ImGuiIO& io = ImGui::GetIO();
    io.KeyMap[ImGuiKey_Tab] = SDLK_TAB;                 // Keyboard mapping. ImGui will use those indices to peek into the io.KeyDown[] array.
    io.KeyMap[ImGuiKey_LeftArrow] = SDL_SCANCODE_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = SDL_SCANCODE_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = SDL_SCANCODE_UP;
    io.KeyMap[ImGuiKey_DownArrow] = SDL_SCANCODE_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = SDL_SCANCODE_PAGEUP;
    io.KeyMap[ImGuiKey_PageDown] = SDL_SCANCODE_PAGEDOWN;
    io.KeyMap[ImGuiKey_Home] = SDL_SCANCODE_HOME;
    io.KeyMap[ImGuiKey_End] = SDL_SCANCODE_END;
    io.KeyMap[ImGuiKey_Delete] = SDLK_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = SDLK_BACKSPACE;
    io.KeyMap[ImGuiKey_Enter] = SDLK_RETURN;
    io.KeyMap[ImGuiKey_Escape] = SDLK_ESCAPE;
    io.KeyMap[ImGuiKey_A] = SDLK_a;
    io.KeyMap[ImGuiKey_C] = SDLK_c;
    io.KeyMap[ImGuiKey_V] = SDLK_v;
    io.KeyMap[ImGuiKey_X] = SDLK_x;
    io.KeyMap[ImGuiKey_Y] = SDLK_y;
    io.KeyMap[ImGuiKey_Z] = SDLK_z;

    io.IniFilename = "";
    io.LogFilename = "";
	
    io.RenderDrawListsFn = ImGui_ImplSdl_RenderDrawLists;
    io.SetClipboardTextFn = ImGui_ImplSdl_SetClipboardText;
    io.GetClipboardTextFn = ImGui_ImplSdl_GetClipboardText;

    return true;
}

void ImGui_ImplSdl_NewFrame(SDL_Window *window)
{
    if (!g_FontTexture)
        ImGui_ImplSdl_CreateDeviceObjects();

    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    int w, h;
	SDL_GetWindowSize(window, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);

    // Setup time step
	Uint32	time = SDL_GetTicks();
	double current_time = time / 1000.0;
    io.DeltaTime = g_Time > 0.0 ? (float)(current_time - g_Time) : (float)(1.0f/60.0f);
    g_Time = current_time;

    // Setup inputs
    // (we already got mouse wheel, keyboard keys & characters from gl.fw callbacks polled in gl.fwPollEvents())
    int mx, my;
    Uint32 mouseMask = SDL_GetMouseState(&mx, &my);
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MOUSE_FOCUS)
    	io.MousePos = ImVec2((float)mx, (float)my);   // Mouse position, in pixels (set to -1,-1 if no mouse / on another screen, etc.)
    else
    	io.MousePos = ImVec2(-1,-1);
   
	io.MouseDown[0] = g_MousePressed[0] || (mouseMask & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;		// If a mouse press event came, always pass it as "mouse held this frame", so we don't miss click-release events that are shorter than 1 frame.
	io.MouseDown[1] = g_MousePressed[1] || (mouseMask & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
	io.MouseDown[2] = g_MousePressed[2] || (mouseMask & SDL_BUTTON(SDL_BUTTON_MIDDLE)) != 0;
    g_MousePressed[0] = g_MousePressed[1] = g_MousePressed[2] = false;

    io.MouseWheel = g_MouseWheel;
    g_MouseWheel = 0.0f;

    // Hide OS mouse cursor if ImGui is drawing it
    SDL_ShowCursor(io.MouseDrawCursor ? 0 : 1);

    // Start the frame
    ImGui::NewFrame();
}

void ImGui_ImplSdl_Shutdown()
{
    if (g_VaoHandle) gl.DeleteVertexArrays(1, &g_VaoHandle);
    if (g_VboHandle) gl.DeleteBuffers(1, &g_VboHandle);
    g_VaoHandle = 0;
    g_VboHandle = 0;

    gl.DetachShader(g_ShaderHandle, g_VertHandle);
    gl.DeleteShader(g_VertHandle);
    g_VertHandle = 0;

    gl.DetachShader(g_ShaderHandle, g_FragHandle);
    gl.DeleteShader(g_FragHandle);
    g_FragHandle = 0;

    gl.DeleteProgram(g_ShaderHandle);
    g_ShaderHandle = 0;

    if (g_FontTexture)
    {
        gl.DeleteTextures(1, &g_FontTexture);
        ImGui::GetIO().Fonts->TexID = 0;
        g_FontTexture = 0;
    }
    g_VboSize = 0;

    ImGui::Shutdown();
}
