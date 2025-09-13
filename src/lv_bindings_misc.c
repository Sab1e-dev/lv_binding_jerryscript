﻿
/**
 * @file lv_bindings_misc.c
 * @brief 模块功能说明
 * @author Sab1e
 * @date 2025-8-10
 */

#include "lv_bindings_misc.h"
#include "lv_bindings.h"
#include <stdlib.h>
#include "uthash.h"
/********************************** 错误处理辅助函数 **********************************/
static jerry_value_t throw_error(const char *message)
{
    jerry_value_t error_obj = jerry_error_sz(JERRY_ERROR_TYPE, (const jerry_char_t *)message);
    return jerry_throw_value(error_obj, true);
}

/********************************** 回调系统 **********************************/
#define MAX_CALLBACKS_PER_KEY 8

// 组合键结构体
typedef struct
{
    lv_obj_t *obj;
    int event;
} callback_key_t;

// 回调映射表结构体，支持多个 JS 回调
typedef struct
{
    callback_key_t key;
    jerry_value_t callbacks[MAX_CALLBACKS_PER_KEY];
    int callback_count;
    UT_hash_handle hh;
} callback_map_t;

static callback_map_t *callback_table = NULL;

/**
 * @brief 处理 LVGL 的事件回调
 * @param e 由 LVGL 传入的事件对象
 */
static void lv_event_handler(lv_event_t *e)
{

    lv_obj_t *target = lv_event_get_target(e);
    int event = lv_event_get_code(e);

    callback_map_t *entry = NULL;
    callback_key_t key = {.obj = target, .event = event};
    HASH_FIND(hh, callback_table, &key, sizeof(callback_key_t), entry);

    if (!entry)
    {
        key.event = LV_EVENT_ALL;
        HASH_FIND(hh, callback_table, &key, sizeof(callback_key_t), entry);
    }
    if (!entry)
        return;

    // 创建事件对象
    jerry_value_t event_obj = jerry_object();
    jerry_value_t global = jerry_current_realm();
    jerry_value_t args[1] = {event_obj};

    // 添加标准属性
    jerry_value_t prop_name, prop_value;

    prop_name = jerry_string_sz("__ptr");
    prop_value = jerry_number((uintptr_t)target);
    jerry_value_free(jerry_object_set(event_obj, prop_name, prop_value));
    jerry_value_free(prop_name);
    jerry_value_free(prop_value);

    prop_name = jerry_string_sz("__type");
    prop_value = jerry_string_sz("lv_event");
    jerry_object_set(event_obj, prop_name, prop_value);
    jerry_value_free(prop_name);
    jerry_value_free(prop_value);

    prop_name = jerry_string_sz("__event_ptr");
    prop_value = jerry_number((uintptr_t)e);
    jerry_object_set(event_obj, prop_name, prop_value);
    jerry_value_free(prop_name);
    jerry_value_free(prop_value);

    prop_name = jerry_string_sz("type");
    prop_value = jerry_number(event);
    jerry_object_set(event_obj, prop_name, prop_value);
    jerry_value_free(prop_name);
    jerry_value_free(prop_value);

    // 添加用户数据（如果存在）
    void *user_data = lv_event_get_user_data(e);
    if (user_data)
    {
        prop_name = jerry_string_sz("user_data");
        prop_value = jerry_number((uintptr_t)user_data);
        jerry_object_set(event_obj, prop_name, prop_value);
        jerry_value_free(prop_name);
        jerry_value_free(prop_value);
    }

    for (int i = 0; i < entry->callback_count; i++)
    {
        jerry_value_t ret = jerry_call(entry->callbacks[i], global, args, 1);
        if (jerry_value_is_error(ret))
        {
            // 处理错误
        }
        jerry_value_free(ret);
    }

    jerry_value_free(global);
    jerry_value_free(event_obj);
}

/**
 * @brief 注册 LVGL 事件处理函数
 * @param args[0] lv_obj_t 对象
 * @param args[1] LVGL 事件类型（整数）
 * @param args[2] JavaScript 函数作为事件处理器
 * @param args[3] （可选） 传入 LVGL 对象的 user_data ，如果留空默认是传入对象的 user_data
 * @return 无返回或抛出异常
 */
static jerry_value_t register_lv_event_handler(const jerry_call_info_t *call_info_p,
                                               const jerry_value_t args[],
                                               const jerry_length_t arg_cnt)
{
    if (arg_cnt < 3 || !jerry_value_is_object(args[0]) ||
        !jerry_value_is_number(args[1]) || !jerry_value_is_function(args[2]))
    {
        return throw_error("Invalid arguments");
    }

    jerry_value_t ptr_val = jerry_object_get(args[0], jerry_string_sz("__ptr"));
    if (!jerry_value_is_number(ptr_val))
    {
        jerry_value_free(ptr_val);
        return throw_error("Invalid __ptr");
    }
    lv_obj_t *obj = (lv_obj_t *)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);

    int event = (int)jerry_value_as_number(args[1]);
    jerry_value_t js_func = jerry_value_copy(args[2]);

    // 自动捕获父对象作为 user_data
    void *user_data = obj; // 默认使用事件目标对象本身

    // 如果回调函数有闭包变量，尝试获取第一个参数作为 user_data
    if (arg_cnt >= 4 && !jerry_value_is_undefined(args[3]))
    {
        if (jerry_value_is_object(args[3]))
        {
            jerry_value_t user_ptr_val = jerry_object_get(args[3], jerry_string_sz("__ptr"));
            if (jerry_value_is_number(user_ptr_val))
            {
                user_data = (void *)(uintptr_t)jerry_value_as_number(user_ptr_val);
            }
            jerry_value_free(user_ptr_val);
        }
        else if (jerry_value_is_number(args[3]))
        {
            user_data = (void *)(uintptr_t)jerry_value_as_number(args[3]);
        }
    }

    callback_map_t *entry = NULL;
    callback_key_t key = {.obj = obj, .event = event};
    HASH_FIND(hh, callback_table, &key, sizeof(callback_key_t), entry);

    if (!entry)
    {
        entry = malloc(sizeof(callback_map_t));
        entry->key = key;
        entry->callback_count = 0;
        memset(entry->callbacks, 0, sizeof(entry->callbacks));
        HASH_ADD(hh, callback_table, key, sizeof(callback_key_t), entry);
        lv_obj_add_event_cb(obj, lv_event_handler, event, user_data);
    }

    if (entry->callback_count < MAX_CALLBACKS_PER_KEY)
    {
        entry->callbacks[entry->callback_count++] = js_func;
    }
    else
    {
        jerry_value_free(js_func);
        return throw_error("Too many callbacks");
    }

    return jerry_undefined();
}
/**
 * @brief 取消注册 LVGL 事件处理函数
 * @param args[0] lv_obj_t 对象
 * @param args[1] LVGL 事件类型（整数）
 * @return 无返回或抛出异常
 */
static jerry_value_t unregister_lv_event_handler(const jerry_call_info_t *call_info_p,
                                                 const jerry_value_t args[],
                                                 const jerry_length_t arg_cnt)
{
    if (arg_cnt < 2 || !jerry_value_is_object(args[0]) || !jerry_value_is_number(args[1]))
    {
        return throw_error("Invalid arguments");
    }

    jerry_value_t ptr_val = jerry_object_get(args[0], jerry_string_sz("__ptr"));
    if (!jerry_value_is_number(ptr_val))
    {
        jerry_value_free(ptr_val);
        return throw_error("Invalid __ptr");
    }
    lv_obj_t *obj = (lv_obj_t *)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);

    int event = (int)jerry_value_as_number(args[1]);

    callback_map_t *entry = NULL;
    callback_key_t key = {.obj = obj, .event = event};
    HASH_FIND(hh, callback_table, &key, sizeof(callback_key_t), entry);

    if (entry)
    {
        for (int i = 0; i < entry->callback_count; i++)
        {
            jerry_value_free(entry->callbacks[i]);
        }
        HASH_DEL(callback_table, entry);
        free(entry);
    }

    return jerry_undefined();
}

/**
 * @brief 当 LVGL 对象被删除时，清理回调映射表中的对应条目
 * @param e 由 LVGL 传入的事件对象
 */
static void lv_obj_deleted_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    callback_map_t *cur, *tmp;
    HASH_ITER(hh, callback_table, cur, tmp)
    {
        if (cur->key.obj == obj)
        {
            for (int i = 0; i < cur->callback_count; i++)
            {
                jerry_value_free(cur->callbacks[i]);
            }
            HASH_DEL(callback_table, cur);
            free(cur);
        }
    }
}

/********************************** 定时器系统 **********************************/

// 定时器数据结构
typedef struct {
    lv_timer_t* timer;
    jerry_value_t js_cb;
    jerry_value_t user_data;
} timer_js_data_t;

/**
 * @brief LVGL 定时器回调包装函数
 * @param timer LVGL 定时器对象
 */     
static void lv_timer_js_cb(lv_timer_t* timer) {
    timer_js_data_t* data = (timer_js_data_t*)lv_timer_get_user_data(timer);
    
    if (data && !jerry_value_is_undefined(data->js_cb)) {
        jerry_value_t global = jerry_current_realm();
        jerry_value_t args[1] = { data->user_data };
        
        jerry_value_t ret = jerry_call(data->js_cb, global, args, 1);
        if (jerry_value_is_error(ret)) {
            
        }
        jerry_value_free(ret);
        jerry_value_free(global);
    }
}

/**
 * @brief 创建 LVGL 定时器
 * @param args[0] JavaScript 函数作为定时器回调
 * @param args[1] 定时器周期（毫秒）
 * @param args[2] （可选）用户数据
 * @return 定时器对象或抛出异常
 */
static jerry_value_t js_lv_timer_create(const jerry_call_info_t* call_info_p,
                                        const jerry_value_t args[],
                                        const jerry_length_t arg_cnt) {
    if (arg_cnt < 2 || !jerry_value_is_function(args[0]) || !jerry_value_is_number(args[1])) {
        return throw_error("Invalid arguments");
    }

    // 获取定时器周期
    uint32_t period = (uint32_t)jerry_value_as_number(args[1]);
    
    // 准备用户数据
    jerry_value_t user_data = jerry_undefined();
    if (arg_cnt >= 3) {
        user_data = jerry_value_copy(args[2]);
    }
    
    // 创建 JavaScript 回调的副本
    jerry_value_t js_cb = jerry_value_copy(args[0]);
    
    // 创建定时器数据结构
    timer_js_data_t* timer_data = (timer_js_data_t*)malloc(sizeof(timer_js_data_t));
    if (!timer_data) {
        jerry_value_free(js_cb);
        jerry_value_free(user_data);
        return throw_error("Failed to allocate memory for timer data");
    }
    
    timer_data->js_cb = js_cb;
    timer_data->user_data = user_data;
    
    // 创建 LVGL 定时器
    lv_timer_t* timer = lv_timer_create(lv_timer_js_cb, period, timer_data);
    if (!timer) {
        free(timer_data);
        return throw_error("Failed to create timer");
    }
    
    timer_data->timer = timer;
    
    // 创建 JavaScript 定时器对象
    jerry_value_t js_timer = jerry_object();
    
    // 添加指针属性
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_number((uintptr_t)timer);
    jerry_object_set(js_timer, ptr_prop, ptr_val);
    jerry_value_free(ptr_prop);
    jerry_value_free(ptr_val);
    
    // 添加类型标记
    jerry_value_t type_prop = jerry_string_sz("__type");
    jerry_value_t type_val = jerry_string_sz("lv_timer");
    jerry_object_set(js_timer, type_prop, type_val);
    jerry_value_free(type_prop);
    jerry_value_free(type_val);
    
    return js_timer;
}

/**
 * @brief 删除 LVGL 定时器
 * @param args[0] 定时器对象
 * @return 无返回或抛出异常
 */
static jerry_value_t js_lv_timer_delete(const jerry_call_info_t* call_info_p,
                                       const jerry_value_t args[],
                                       const jerry_length_t arg_cnt) {
    if (arg_cnt < 1 || !jerry_value_is_object(args[0])) {
        return throw_error("Invalid arguments");
    }
    
    // 获取定时器指针
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);
    
    if (!jerry_value_is_number(ptr_val)) {
        jerry_value_free(ptr_val);
        return throw_error("Invalid timer object");
    }
    
    lv_timer_t* timer = (lv_timer_t*)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);
    
    // 获取定时器数据
    timer_js_data_t* timer_data = (timer_js_data_t*)lv_timer_get_user_data(timer);
    
    // 释放 JavaScript 资源
    if (timer_data) {
        jerry_value_free(timer_data->js_cb);
        jerry_value_free(timer_data->user_data);
        free(timer_data);
    }
    
    // 删除定时器
    lv_timer_del(timer);
    
    return jerry_undefined();
}

/**
 * @brief 设置定时器周期
 * @param args[0] 定时器对象
 * @param args[1] 新的周期（毫秒）
 * @return 无返回或抛出异常
 */
static jerry_value_t js_lv_timer_set_period(const jerry_call_info_t* call_info_p,
                                           const jerry_value_t args[],
                                           const jerry_length_t arg_cnt) {
    if (arg_cnt < 2 || !jerry_value_is_object(args[0]) || !jerry_value_is_number(args[1])) {
        return throw_error("Invalid arguments");
    }
    
    // 获取定时器指针
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);
    
    if (!jerry_value_is_number(ptr_val)) {
        jerry_value_free(ptr_val);
        return throw_error("Invalid timer object");
    }
    
    lv_timer_t* timer = (lv_timer_t*)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);
    
    // 设置定时器周期
    uint32_t period = (uint32_t)jerry_value_as_number(args[1]);
    lv_timer_set_period(timer, period);
    
    return jerry_undefined();
}

/**
 * @brief 设置定时器重复次数
 * @param args[0] 定时器对象
 * @param args[1] 重复次数（-1表示无限重复）
 * @return 无返回或抛出异常
 */
static jerry_value_t js_lv_timer_set_repeat_count(const jerry_call_info_t* call_info_p,
                                                 const jerry_value_t args[],
                                                 const jerry_length_t arg_cnt) {
    if (arg_cnt < 2 || !jerry_value_is_object(args[0]) || !jerry_value_is_number(args[1])) {
        return throw_error("Invalid arguments");
    }
    
    // 获取定时器指针
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);
    
    if (!jerry_value_is_number(ptr_val)) {
        jerry_value_free(ptr_val);
        return throw_error("Invalid timer object");
    }
    
    lv_timer_t* timer = (lv_timer_t*)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);
    
    // 设置定时器重复次数
    int32_t repeat_count = (int32_t)jerry_value_as_number(args[1]);
    lv_timer_set_repeat_count(timer, repeat_count);
    
    return jerry_undefined();
}

/**
 * @brief 重置定时器
 * @param args[0] 定时器对象
 * @return 无返回或抛出异常
 */
static jerry_value_t js_lv_timer_reset(const jerry_call_info_t* call_info_p,
                                      const jerry_value_t args[],
                                      const jerry_length_t arg_cnt) {
    if (arg_cnt < 1 || !jerry_value_is_object(args[0])) {
        return throw_error("Invalid arguments");
    }
    
    // 获取定时器指针
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);
    
    if (!jerry_value_is_number(ptr_val)) {
        jerry_value_free(ptr_val);
        return throw_error("Invalid timer object");
    }
    
    lv_timer_t* timer = (lv_timer_t*)(uintptr_t)jerry_value_as_number(ptr_val);
    jerry_value_free(ptr_val);
    
    // 重置定时器
    lv_timer_reset(timer);
    
    return jerry_undefined();
}

/********************************** 色彩转换函数 **********************************/
/**
 * @brief 解析JS颜色值到lv_color_t
 */
lv_color_t js_to_lv_color(jerry_value_t js_color)
{
    lv_color_t color = {0}; // 初始化为黑色

    // 处理undefined/null
    if (jerry_value_is_undefined(js_color) || jerry_value_is_null(js_color))
    {
        return color;
    }

    // 获取RGB值
    uint8_t r = 0, g = 0, b = 0;

    // 处理数字输入（直接作为hex值）
    if (jerry_value_is_number(js_color))
    {
        uint32_t hex = (uint32_t)jerry_value_as_number(js_color);
        r = (hex >> 16) & 0xFF;
        g = (hex >> 8) & 0xFF;
        b = hex & 0xFF;
    }
    // 处理对象输入
    else if (jerry_value_is_object(js_color))
    {
        // 优先检查hex属性
        jerry_value_t hex_val = jerry_object_get(js_color, jerry_string_sz("hex"));
        if (jerry_value_is_number(hex_val))
        {
            uint32_t hex = (uint32_t)jerry_value_as_number(hex_val);
            r = (hex >> 16) & 0xFF;
            g = (hex >> 8) & 0xFF;
            b = hex & 0xFF;
        }
        jerry_value_free(hex_val);

        // 其次检查rgb属性
        if (r == 0 && g == 0 && b == 0)
        {
            jerry_value_t r_val = jerry_object_get(js_color, jerry_string_sz("r"));
            jerry_value_t g_val = jerry_object_get(js_color, jerry_string_sz("g"));
            jerry_value_t b_val = jerry_object_get(js_color, jerry_string_sz("b"));

            if (jerry_value_is_number(r_val))
                r = (uint8_t)jerry_value_as_number(r_val);
            if (jerry_value_is_number(g_val))
                g = (uint8_t)jerry_value_as_number(g_val);
            if (jerry_value_is_number(b_val))
                b = (uint8_t)jerry_value_as_number(b_val);

            jerry_value_free(r_val);
            jerry_value_free(g_val);
            jerry_value_free(b_val);
        }
    }

    // 填充颜色结构体（注意顺序：blue, green, red）
    color.blue = b;
    color.green = g;
    color.red = r;

    return color;
}

/**
 * @brief 将lv_color_t转换为JS对象
 */
jerry_value_t lv_color_to_js(lv_color_t color)
{
    jerry_value_t js_color = jerry_object();

    // 添加RGB分量（注意顺序与结构体相反）
    jerry_object_set(js_color, jerry_string_sz("r"), jerry_number(color.red));
    jerry_object_set(js_color, jerry_string_sz("g"), jerry_number(color.green));
    jerry_object_set(js_color, jerry_string_sz("b"), jerry_number(color.blue));

    // 添加十六进制颜色值
    uint32_t hex = (color.red << 16) | (color.green << 8) | color.blue;
    jerry_object_set(js_color, jerry_string_sz("hex"), jerry_number(hex));

    // 标记为LVGL颜色对象
    jerry_object_set(js_color, jerry_string_sz("__type"), jerry_string_sz("lv_color"));

    return js_color;
}
/********************************** 特殊 LVGL 函数 **********************************/
/**
 * @brief 样式初始化
 */
static jerry_value_t js_lv_style_init(const jerry_call_info_t *call_info_p,
                                      const jerry_value_t args[],
                                      const jerry_length_t argc)
{
    // 参数数量检查
    if (argc < 1)
    {
        return throw_error("Insufficient arguments");
    }

    // 检查参数是否为对象
    if (!jerry_value_is_object(args[0]))
    {
        return throw_error("Argument must be a style object");
    }

    // 检查对象是否已经分配了内存
    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);

    lv_style_t *style = NULL;

    if (jerry_value_is_number(ptr_val))
    {
        // 已有指针的情况
        uintptr_t ptr = (uintptr_t)jerry_value_as_number(ptr_val);
        style = (lv_style_t *)ptr;
    }
    else
    {
        // 没有指针的情况，分配新内存
        style = (lv_style_t *)malloc(sizeof(lv_style_t));
        if (!style)
        {
            jerry_value_free(ptr_val);
            return throw_error("Failed to allocate memory for style");
        }

        // 将指针保存回JS对象
        ptr_val = jerry_number((uintptr_t)style);
        ptr_prop = jerry_string_sz("__ptr");
        jerry_object_set(args[0], ptr_prop, ptr_val);
        jerry_value_free(ptr_prop);
        jerry_value_free(ptr_val);

        // 添加类型标记
        jerry_value_t type_prop = jerry_string_sz("__type");
        jerry_value_t type_val = jerry_string_sz("lv_style");
        jerry_object_set(args[0], type_prop, type_val);
        jerry_value_free(type_prop);
        jerry_value_free(type_val);
    }

    // 调用初始化函数
    lv_style_init(style);

    // 返回JS对象本身，支持链式调用
    return jerry_value_copy(args[0]);
}
/**
 * @brief 样式清除
 */
static jerry_value_t js_lv_style_delete(const jerry_call_info_t *call_info_p,
                                        const jerry_value_t args[],
                                        const jerry_length_t argc)
{
    if (argc < 1 || !jerry_value_is_object(args[0]))
    {
        return throw_error("Invalid arguments");
    }

    jerry_value_t ptr_prop = jerry_string_sz("__ptr");
    jerry_value_t ptr_val = jerry_object_get(args[0], ptr_prop);
    jerry_value_free(ptr_prop);

    if (jerry_value_is_number(ptr_val))
    {
        lv_style_t *style = (lv_style_t *)(uintptr_t)jerry_value_as_number(ptr_val);
        free(style);

        // 清除指针引用
        ptr_prop = jerry_string_sz("__ptr");
        jerry_object_set(args[0], ptr_prop, jerry_number(0));
        jerry_value_free(ptr_prop);
    }

    jerry_value_free(ptr_val);
    return jerry_undefined();
}

/********************************** 字体系统 **********************************/
static void register_lvgl_fonts(void)
{
    jerry_value_t global = jerry_current_realm();

    // 创建字体对象容器
    jerry_value_t fonts = jerry_object();

    // 注册Montserrat字体集
#define REGISTER_FONT(name)                                                                   \
    do                                                                                        \
    {                                                                                         \
        jerry_value_t font_obj = jerry_object();                                              \
        jerry_object_set(font_obj, jerry_string_sz("__ptr"), jerry_number((uintptr_t)&name)); \
        jerry_object_set(font_obj, jerry_string_sz("__type"), jerry_string_sz("lv_font"));    \
        jerry_object_set(fonts, jerry_string_sz(#name), font_obj);                            \
        jerry_value_free(font_obj);                                                           \
    } while (0);
#if LV_FONT_MONTSERRAT_8
    REGISTER_FONT(lv_font_montserrat_8);
#endif
#if LV_FONT_MONTSERRAT_10
    REGISTER_FONT(lv_font_montserrat_10);
#endif
#if LV_FONT_MONTSERRAT_12
    REGISTER_FONT(lv_font_montserrat_12);
#endif
#if LV_FONT_MONTSERRAT_14
    REGISTER_FONT(lv_font_montserrat_14);
#endif
#if LV_FONT_MONTSERRAT_16
    REGISTER_FONT(lv_font_montserrat_16);
#endif
#if LV_FONT_MONTSERRAT_18
    REGISTER_FONT(lv_font_montserrat_18);
#endif
#if LV_FONT_MONTSERRAT_20
    REGISTER_FONT(lv_font_montserrat_20);
#endif
#if LV_FONT_MONTSERRAT_22
    REGISTER_FONT(lv_font_montserrat_22);
#endif
#if LV_FONT_MONTSERRAT_24
    REGISTER_FONT(lv_font_montserrat_24);
#endif
#if LV_FONT_MONTSERRAT_26
    REGISTER_FONT(lv_font_montserrat_26);
#endif
#if LV_FONT_MONTSERRAT_28
    REGISTER_FONT(lv_font_montserrat_28);
#endif
#if LV_FONT_MONTSERRAT_30
    REGISTER_FONT(lv_font_montserrat_30);
#endif
#if LV_FONT_MONTSERRAT_32
    REGISTER_FONT(lv_font_montserrat_32);
#endif
#if LV_FONT_MONTSERRAT_34
    REGISTER_FONT(lv_font_montserrat_34);
#endif
#if LV_FONT_MONTSERRAT_36
    REGISTER_FONT(lv_font_montserrat_36);
#endif
#if LV_FONT_MONTSERRAT_38
    REGISTER_FONT(lv_font_montserrat_38);
#endif
#if LV_FONT_MONTSERRAT_40
    REGISTER_FONT(lv_font_montserrat_40);
#endif
#if LV_FONT_MONTSERRAT_42
    REGISTER_FONT(lv_font_montserrat_42);
#endif
#if LV_FONT_MONTSERRAT_44
    REGISTER_FONT(lv_font_montserrat_44);
#endif
#if LV_FONT_MONTSERRAT_46
    REGISTER_FONT(lv_font_montserrat_46);
#endif
#if LV_FONT_MONTSERRAT_48
    REGISTER_FONT(lv_font_montserrat_48);
#endif

#undef REGISTER_FONT

    // 将字体容器挂载到全局对象
    jerry_object_set(global, jerry_string_sz("lv_font"), fonts);
    jerry_value_free(fonts);
    jerry_value_free(global);
}
/********************************** 绑定注册 **********************************/

const LVBindingJerryscriptFuncEntry_t lvgl_binding_special_funcs[] = {
    {"register_lv_event_handler", register_lv_event_handler},
    {"unregister_lv_event_handler", unregister_lv_event_handler},
    {"lv_style_init", js_lv_style_init},
    {"lv_style_delete", js_lv_style_delete},
    {"lv_timer_create", js_lv_timer_create},
    {"lv_timer_delete", js_lv_timer_delete},
    {"lv_timer_set_period", js_lv_timer_set_period},
    {"lv_timer_set_repeat_count", js_lv_timer_set_repeat_count},
    {"lv_timer_reset", js_lv_timer_reset}};

void lv_binding_jerryscript_register_functions(const LVBindingJerryscriptFuncEntry_t *entry, const size_t funcs_count)
{
    jerry_value_t global = jerry_current_realm();
    for (size_t i = 0; i < funcs_count; ++i)
    {
        jerry_value_t fn = jerry_function_external(entry[i].handler);
        jerry_value_t name = jerry_string_sz(entry[i].name);
        jerry_object_set(global, name, fn);
        jerry_value_free(name);
        jerry_value_free(fn);
    }
    jerry_value_free(global);
}
/********************************** 初始化 **********************************/
void lv_bindings_misc_init(void)
{
    // 初始化函数
    lv_obj_add_event_cb(lv_scr_act(), lv_obj_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_binding_jerryscript_register_functions(lvgl_binding_special_funcs, sizeof(lvgl_binding_special_funcs) / sizeof(LVBindingJerryscriptFuncEntry_t));
    register_lvgl_fonts();
}
