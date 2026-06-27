#pragma once

struct app_context;

class panel_base {
public:
    bool visible = false;

    virtual void init(app_context* ctx) { ctx_ = ctx; }
    virtual void pre_frame() {}   // called before any ImGui::Begin() this frame
    virtual void render()    = 0;
    virtual ~panel_base()    = default;

protected:
    app_context* ctx_ = nullptr;
};
