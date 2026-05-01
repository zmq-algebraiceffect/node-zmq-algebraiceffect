#include <napi.h>
#include <uv.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" {
#include <zmqae/zmqae.h>
}

namespace {

// --- Client ---

struct client_perform_data {
    Napi::Promise::Deferred deferred;
    Napi::Env env;
    zmqae_binary_t *owned_bins{nullptr};
};

void client_perform_callback(void *user_data,
                              const char *id,
                              const char *json_value,
                              const char *error_message) {
    auto *data = static_cast<client_perform_data *>(user_data);
    Napi::HandleScope scope(data->env);

    Napi::Object result = Napi::Object::New(data->env);
    result.Set("id", Napi::String::New(data->env, id ? id : ""));

    if (error_message) {
        result.Set("error", Napi::String::New(data->env, error_message));
    }
    if (json_value) {
        Napi::Value parsed = data->env.Global().Get("JSON").As<Napi::Object>()
            .Get("parse").As<Napi::Function>()
            .Call({Napi::String::New(data->env, json_value)}).As<Napi::Value>();
        result.Set("value", parsed);
    }

    if (error_message) {
        data->deferred.Reject(Napi::Error::New(data->env, error_message).Value());
    } else {
        data->deferred.Resolve(result);
    }

    if (data->owned_bins) {
        delete[] data->owned_bins;
    }
    delete data;
}

class ClientWrapper : public Napi::ObjectWrap<ClientWrapper> {
public:
    static void init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "Client", {
            InstanceMethod("perform", &ClientWrapper::perform),
            InstanceMethod("close", &ClientWrapper::close),
        });
        exports.Set("Client", func);
    }

    explicit ClientWrapper(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<ClientWrapper>(info)
    {
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(info.Env(), "Expected endpoint string").ThrowAsJavaScriptException();
            return;
        }
        std::string endpoint = info[0].As<Napi::String>().Utf8Value();
        client_ = zmqae_client_new(endpoint.c_str());
        if (!client_) {
            Napi::Error::New(info.Env(), zmqae_last_error()).ThrowAsJavaScriptException();
            return;
        }
        start_poll_timer();
    }

    ~ClientWrapper() {
        stop_poll_timer();
        if (client_) {
            zmqae_client_destroy(client_);
            client_ = nullptr;
        }
    }

    Napi::Value perform(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (info.Length() < 2) {
            Napi::TypeError::New(env, "perform requires (effect, payload[, ...])").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (!info[0].IsString()) {
            Napi::TypeError::New(env, "effect must be a string").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string effect = info[0].As<Napi::String>().Utf8Value();

        std::string json_payload = "{}";
        if (info[1].IsUndefined() || info[1].IsNull()) {
            // keep "{}"
        } else if (info[1].IsString()) {
            json_payload = info[1].As<Napi::String>().Utf8Value();
        } else if (info[1].IsObject()) {
            json_payload = env.Global().Get("JSON").As<Napi::Object>()
                .Get("stringify").As<Napi::Function>()
                .Call({info[1]}).As<Napi::String>().Utf8Value();
        } else {
            Napi::TypeError::New(env, "payload must be string or object").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int timeout_ms = 0;
        int binaries_index = -1;

        for (int i = 2; i < info.Length(); ++i) {
            if (info[i].IsArray()) {
                binaries_index = i;
            } else if (info[i].IsNumber()) {
                timeout_ms = info[i].As<Napi::Number>().Int32Value();
            }
        }

        auto *data = new client_perform_data{Napi::Promise::Deferred::New(env), env, nullptr};

        if (binaries_index >= 0) {
            Napi::Array bins_arr = info[binaries_index].As<Napi::Array>();
            int bin_count = static_cast<int>(bins_arr.Length());
            data->owned_bins = new zmqae_binary_t[bin_count]();
            for (int i = 0; i < bin_count; ++i) {
                Napi::Buffer<uint8_t> buf = bins_arr.Get(i).As<Napi::Buffer<uint8_t>>();
                data->owned_bins[i].data = buf.Data();
                data->owned_bins[i].size = static_cast<int>(buf.Length());
            }

            auto rc = zmqae_client_perform_binary(
                client_, effect.c_str(), json_payload.c_str(),
                data->owned_bins, bin_count, timeout_ms,
                client_perform_callback, data);

            if (rc != ZMQAE_OK) {
                delete[] data->owned_bins;
                delete data;
                auto deferred = Napi::Promise::Deferred::New(env);
                deferred.Reject(Napi::Error::New(env, zmqae_last_error()).Value());
                return deferred.Promise();
            }
        } else {
            int rc;
            if (timeout_ms > 0) {
                rc = zmqae_client_perform_timeout(
                    client_, effect.c_str(), json_payload.c_str(),
                    timeout_ms, client_perform_callback, data);
            } else {
                rc = zmqae_client_perform(
                    client_, effect.c_str(), json_payload.c_str(),
                    client_perform_callback, data);
            }

            if (rc != ZMQAE_OK) {
                delete data;
                auto deferred = Napi::Promise::Deferred::New(env);
                deferred.Reject(Napi::Error::New(env, zmqae_last_error()).Value());
                return deferred.Promise();
            }
        }

        return data->deferred.Promise();
    }

    Napi::Value close(const Napi::CallbackInfo &info) {
        if (closed_.exchange(true)) {
            return info.Env().Undefined();
        }
        stop_poll_timer();
        if (client_) {
            zmqae_client_close(client_);
        }
        return info.Env().Undefined();
    }

private:
    void start_poll_timer() {
        uv_timer_init(uv_default_loop(), &timer_);
        timer_.data = this;
        uv_timer_start(&timer_, on_uv_timer, 0, 1);
    }

    void stop_poll_timer() {
        uv_timer_stop(&timer_);
    }

    static void on_uv_timer(uv_timer_t *handle) {
        auto *self = static_cast<ClientWrapper *>(handle->data);
        if (self->closed_.load()) {
            uv_timer_stop(handle);
            return;
        }
        zmqae_client_poll(self->client_);
    }

    zmqae_client_t *client_{nullptr};
    uv_timer_t timer_{};
    std::atomic<bool> closed_{false};
};

// --- Router ---

struct router_handler_data {
    std::shared_ptr<Napi::FunctionReference> handler_ref;
    Napi::Env env;
};

void router_handler_fn(void *user_data, zmqae_perform_ctx_t *ctx) {
    auto *data = static_cast<router_handler_data *>(user_data);
    Napi::HandleScope scope(data->env);

    Napi::Function handler = data->handler_ref->Value();

    Napi::Object ctx_obj = Napi::Object::New(data->env);
    ctx_obj.Set("id", Napi::String::New(data->env, zmqae_ctx_get_id(ctx)));
    ctx_obj.Set("effect", Napi::String::New(data->env, zmqae_ctx_get_effect(ctx)));

    const char *json_str = zmqae_ctx_get_payload(ctx);
    Napi::Value parsed = data->env.Global().Get("JSON").As<Napi::Object>()
        .Get("parse").As<Napi::Function>()
        .Call({Napi::String::New(data->env, json_str)}).As<Napi::Value>();
    ctx_obj.Set("payload", parsed);

    ctx_obj.DefineProperty(Napi::PropertyDescriptor::Function(
        "resume", [ctx](const Napi::CallbackInfo &info) -> Napi::Value {
            Napi::Env env = info.Env();
            std::string json_value = "{}";
            if (info.Length() > 0 && !info[0].IsUndefined() && !info[0].IsNull()) {
                if (info[0].IsObject()) {
                    json_value = env.Global().Get("JSON").As<Napi::Object>()
                        .Get("stringify").As<Napi::Function>()
                        .Call({info[0]}).As<Napi::String>().Utf8Value();
                } else if (info[0].IsString()) {
                    json_value = info[0].As<Napi::String>().Utf8Value();
                }
            }
            zmqae_ctx_resume(ctx, json_value.c_str());
            return env.Undefined();
        }));

    ctx_obj.DefineProperty(Napi::PropertyDescriptor::Function(
        "error", [ctx](const Napi::CallbackInfo &info) -> Napi::Value {
            std::string error_msg = "unknown error";
            if (info.Length() > 0 && info[0].IsString()) {
                error_msg = info[0].As<Napi::String>().Utf8Value();
            }
            zmqae_ctx_error(ctx, error_msg.c_str());
            return info.Env().Undefined();
        }));

    handler.Call({ctx_obj});
    // Note: ctx is NOT released here.
    // The shared_ptr inside ctx keeps the perform_context alive.
    // The handler must call ctx.resume() or ctx.error() to respond.
    // If the handler doesn't respond, the context stays alive until the
    // client times out and the router eventually cleans up.
}

class RouterWrapper : public Napi::ObjectWrap<RouterWrapper> {
public:
    static void init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "Router", {
            InstanceMethod("on", &RouterWrapper::on),
            InstanceMethod("off", &RouterWrapper::off),
            InstanceMethod("close", &RouterWrapper::close),
        });
        exports.Set("Router", func);
    }

    explicit RouterWrapper(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<RouterWrapper>(info)
    {
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(info.Env(), "Expected endpoint string").ThrowAsJavaScriptException();
            return;
        }
        std::string endpoint = info[0].As<Napi::String>().Utf8Value();
        router_ = zmqae_router_new(endpoint.c_str());
        if (!router_) {
            Napi::Error::New(info.Env(), zmqae_last_error()).ThrowAsJavaScriptException();
            return;
        }
        start_poll_timer();
    }

    ~RouterWrapper() {
        stop_poll_timer();
        if (router_) {
            zmqae_router_destroy(router_);
            router_ = nullptr;
        }
    }

    Napi::Value on(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
            Napi::TypeError::New(env, "on requires (effect, handler)").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string effect = info[0].As<Napi::String>().Utf8Value();

        auto ref = std::make_shared<Napi::FunctionReference>();
        *ref = Napi::Persistent(info[1].As<Napi::Function>());

        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler_refs_[effect] = ref;
        }

        auto *data = new router_handler_data{ref, env};

        auto rc = zmqae_router_on(router_, effect.c_str(), router_handler_fn, data);
        if (rc != ZMQAE_OK) {
            delete data;
            Napi::Error::New(env, zmqae_last_error()).ThrowAsJavaScriptException();
        }

        return env.Undefined();
    }

    Napi::Value off(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();

        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "off requires (effect)").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string effect = info[0].As<Napi::String>().Utf8Value();
        zmqae_router_off(router_, effect.c_str());

        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler_refs_.erase(effect);
        }

        return env.Undefined();
    }

    Napi::Value close(const Napi::CallbackInfo &info) {
        if (closed_.exchange(true)) {
            return info.Env().Undefined();
        }
        stop_poll_timer();
        if (router_) {
            zmqae_router_close(router_);
        }
        return info.Env().Undefined();
    }

private:
    void start_poll_timer() {
        uv_timer_init(uv_default_loop(), &timer_);
        timer_.data = this;
        uv_timer_start(&timer_, on_uv_timer, 0, 1);
    }

    void stop_poll_timer() {
        uv_timer_stop(&timer_);
    }

    static void on_uv_timer(uv_timer_t *handle) {
        auto *self = static_cast<RouterWrapper *>(handle->data);
        if (self->closed_.load()) {
            uv_timer_stop(handle);
            return;
        }
        zmqae_router_poll(self->router_);
    }

    zmqae_router_t *router_{nullptr};
    uv_timer_t timer_{};
    std::unordered_map<std::string, std::shared_ptr<Napi::FunctionReference>> handler_refs_;
    std::mutex handlers_mutex_;
    std::atomic<bool> closed_{false};
};

} // anonymous namespace

Napi::Object init_all(Napi::Env env, Napi::Object exports) {
    ClientWrapper::init(env, exports);
    RouterWrapper::init(env, exports);
    return exports;
}

NODE_API_MODULE(zmq_algebraiceffect, init_all)
