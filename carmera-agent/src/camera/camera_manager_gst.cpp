// Real camera enumeration via GStreamer's GstDeviceMonitor.
// Compiled only when CAMERA_AGENT_BACKEND == gstreamer.
#include "camera_agent/camera_manager.h"
#include "camera_agent/logger.h"

#include <gst/gst.h>

#include <memory>
#include <set>
#include <string>

namespace ca {

class GstCameraManager : public CameraManager {
public:
    GstCameraManager() {
        if (!gst_is_initialized())
            gst_init(nullptr, nullptr);
    }

    std::vector<CameraInfo> enumerate() override {
        std::vector<CameraInfo> out;

        GstDeviceMonitor* mon = gst_device_monitor_new();
        if (!mon) {
            CA_LOG_ERROR("Failed to create GstDeviceMonitor");
            return out;
        }

        GstCaps* filter = gst_caps_new_empty_simple("video/x-raw");
        gst_device_monitor_add_filter(mon, "Video/Source", filter);
        gst_caps_unref(filter);

        if (gst_device_monitor_start(mon) == FALSE) {
            CA_LOG_ERROR("Failed to start GstDeviceMonitor (no camera bus?)");
            gst_object_unref(mon);
            return out;
        }

        GList* devices = gst_device_monitor_get_devices(mon);
        int id = 0;
        for (GList* l = devices; l != nullptr; l = l->next) {
            GstDevice* dev = static_cast<GstDevice*>(l->data);
            gchar* display = gst_device_get_display_name(dev);

            CameraInfo info;
            info.id = id++;
            info.name = (display ? display : "Unknown");
            if (display) g_free(display);

            GstCaps* caps = gst_device_get_caps(dev);
            parse_caps(caps, info);
            if (caps) gst_caps_unref(caps);

            out.push_back(std::move(info));
        }

        g_list_free_full(devices, g_object_unref);
        gst_device_monitor_stop(mon);
        gst_object_unref(mon);
        return out;
    }

    bool is_available(int id) const override {
        // Cheap sanity check; full validation happens during pipeline build.
        return id >= 0;
    }

    std::string backend_name() const override {
        return "gstreamer";
    }

private:
    static void parse_caps(GstCaps* caps, CameraInfo& info) {
        std::set<std::pair<int, int>> resolutions;
        std::set<int> fps;

        const guint n = (caps ? gst_caps_get_size(caps) : 0);
        for (guint i = 0; i < n; ++i) {
            const GstStructure* s = gst_caps_get_structure(caps, i);

            gint w = 0, h = 0;
            if (gst_structure_get_int(s, "width", &w) &&
                gst_structure_get_int(s, "height", &h)) {
                resolutions.insert({w, h});
            }

            const GValue* fr = gst_structure_get_value(s, "framerate");
            if (fr && GST_VALUE_HOLDS_FRACTION(fr)) {
                const gint num = gst_value_get_fraction_numerator(fr);
                const gint den = gst_value_get_fraction_denominator(fr);
                if (den > 0 && num > 0)
                    fps.insert(num / den);
            }
        }

        for (const auto& r : resolutions)
            info.resolutions.push_back({r.first, r.second});
        for (int f : fps)
            info.fps.push_back(f);
    }
};

std::unique_ptr<CameraManager> CameraManager::create() {
    return std::make_unique<GstCameraManager>();
}

} // namespace ca
