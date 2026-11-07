#pragma once
#include <string>
namespace reshade::api {struct effect_runtime;}
namespace qs {
void keys_start();
void keys_stop();
bool quick_key_pressed(reshade::api::effect_runtime *rt);
void draw_quick_keys(reshade::api::effect_runtime *rt);
std::string quick_keys_label();
}
