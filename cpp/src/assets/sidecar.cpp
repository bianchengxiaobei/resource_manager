#include "parcel/assets/sidecar.hpp"

#include "parcel/core/error.hpp"
#include "parcel/core/platform_file.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace parcel::assets {
namespace {

using core::ErrorCode;
using core::MakeError;

core::Error Fail(std::string_view context, std::string message) {
    return MakeError(ErrorCode::InvalidArgument, std::string(context) + ": " + std::move(message));
}

Result<float> ReadFloat(const JsonValue& value, std::string_view context) {
    if (!value.IsNumber() || !std::isfinite(value.number_value) ||
        std::abs(value.number_value) > std::numeric_limits<float>::max()) {
        return Fail(context, "expected a finite number");
    }
    return static_cast<float>(value.number_value);
}

Result<std::uint32_t> ReadUInt(const JsonValue& value, std::string_view context) {
    if (!value.IsNumber() || value.number_value < 0.0 ||
        value.number_value > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(value.number_value) != value.number_value) {
        return Fail(context, "expected an unsigned integer");
    }
    return static_cast<std::uint32_t>(value.number_value);
}

Result<std::int32_t> ReadInt(const JsonValue& value, std::string_view context) {
    if (!value.IsNumber() ||
        value.number_value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        value.number_value > static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
        std::floor(value.number_value) != value.number_value) {
        return Fail(context, "expected an integer");
    }
    return static_cast<std::int32_t>(value.number_value);
}

Result<std::string> ReadString(const JsonValue& value, std::string_view context) {
    if (!value.IsString()) {
        return Fail(context, "expected a string");
    }
    return value.string_value;
}

Result<Vec4> ReadVec4(const JsonValue& value, std::string_view context) {
    if (!value.IsArray() || value.array_value.size() != 4) {
        return Fail(context, "expected an array of four numbers");
    }
    Vec4 result;
    PARCEL_TRY_ASSIGN(result.x, ReadFloat(value.array_value[0], context));
    PARCEL_TRY_ASSIGN(result.y, ReadFloat(value.array_value[1], context));
    PARCEL_TRY_ASSIGN(result.z, ReadFloat(value.array_value[2], context));
    PARCEL_TRY_ASSIGN(result.w, ReadFloat(value.array_value[3], context));
    return result;
}

Result<SidecarProjection> ReadProjection(const JsonValue& value, std::string_view context) {
    Result<std::string> text = ReadString(value, context);
    if (!text) {
        return std::move(text).TakeError();
    }
    if (text.Value() == "perspective") {
        return SidecarProjection::Perspective;
    }
    if (text.Value() == "orthographic") {
        return SidecarProjection::Orthographic;
    }
    return Fail(context, "expected perspective or orthographic");
}

Result<void> ParseSettings(const JsonValue& value, SidecarSettings& settings) {
    if (!value.IsObject()) {
        return Fail("settings", "expected an object");
    }
    const std::pair<std::string_view, std::string*> fields[] = {
        {"pipeline", &settings.pipeline},        {"frame", &settings.frame},
        {"postProcess", &settings.post_process}, {"postprocess", &settings.post_process},
        {"shadows", &settings.shadows},          {"volumetricFog", &settings.volumetric_fog},
    };
    for (const auto& [key, target] : fields) {
        const JsonValue* field = value.Find(key);
        if (field == nullptr) {
            continue;
        }
        Result<std::string> text = ReadString(*field, std::string("settings.") + std::string(key));
        if (!text) {
            return std::move(text).TakeError();
        }
        if (text.Value().empty()) {
            return Fail(std::string("settings.") + std::string(key), "path is empty");
        }
        *target = text.Value();
    }
    return {};
}

Result<SidecarCamera> ParseCamera(const JsonValue& value, std::size_t index) {
    if (!value.IsObject()) {
        return Fail("cameras[" + std::to_string(index) + "]", "expected an object");
    }
    SidecarCamera camera;
    if (const JsonValue* field = value.Find("name"); field != nullptr) {
        PARCEL_TRY_ASSIGN(camera.name, ReadString(*field, "cameras[].name"));
    }
    if (const JsonValue* field = value.Find("projection"); field != nullptr) {
        PARCEL_TRY_ASSIGN(camera.projection, ReadProjection(*field, "cameras[].projection"));
    }
    const std::pair<std::string_view, float*> floats[] = {
        {"yfov", &camera.yfov},        {"aspect", &camera.aspect}, {"nearClip", &camera.near_clip},
        {"farClip", &camera.far_clip}, {"xmag", &camera.xmag},     {"ymag", &camera.ymag},
    };
    for (const auto& [key, target] : floats) {
        if (const JsonValue* field = value.Find(key); field != nullptr) {
            PARCEL_TRY_ASSIGN(*target,
                              ReadFloat(*field, std::string("cameras[].") + std::string(key)));
        }
    }
    if (const JsonValue* field = value.Find("clearColor"); field != nullptr) {
        PARCEL_TRY_ASSIGN(camera.clear_color, ReadVec4(*field, "cameras[].clearColor"));
    }
    if (const JsonValue* field = value.Find("cullingMask"); field != nullptr) {
        PARCEL_TRY_ASSIGN(camera.culling_mask, ReadUInt(*field, "cameras[].cullingMask"));
    }
    if (const JsonValue* field = value.Find("priority"); field != nullptr) {
        PARCEL_TRY_ASSIGN(camera.priority, ReadInt(*field, "cameras[].priority"));
    }
    return camera;
}

Result<std::vector<SidecarObject>> ParseObjects(const JsonValue& value, std::string_view context) {
    if (!value.IsArray()) {
        return Fail(context, "expected an array");
    }
    std::vector<SidecarObject> objects;
    objects.reserve(value.array_value.size());
    for (std::size_t i = 0; i < value.array_value.size(); ++i) {
        const JsonValue& entry = value.array_value[i];
        if (!entry.IsObject()) {
            return Fail(std::string(context) + "[" + std::to_string(i) + "]", "expected an object");
        }
        SidecarObject object;
        if (const JsonValue* name = entry.Find("name"); name != nullptr) {
            PARCEL_TRY_ASSIGN(object.name, ReadString(*name, "object name"));
        }
        object.data = entry;
        objects.push_back(std::move(object));
    }
    return objects;
}

} // namespace

Result<SceneSidecar> ParseSceneSidecar(std::string_view text) {
    Result<JsonValue> parsed = ParseJson(text);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    const JsonValue& root = parsed.Value();
    if (!root.IsObject()) {
        return Fail("root", "expected an object");
    }
    const JsonValue* version = root.Find("version");
    if (version == nullptr) {
        return Fail("version", "missing required field");
    }
    std::uint32_t version_value = 0;
    PARCEL_TRY_ASSIGN(version_value, ReadUInt(*version, "version"));
    if (version_value != 1) {
        return MakeError(ErrorCode::Unsupported, "unsupported sidecar version");
    }

    SceneSidecar sidecar;
    sidecar.version = version_value;
    if (const JsonValue* source = root.Find("source"); source != nullptr) {
        PARCEL_TRY_ASSIGN(sidecar.source, ReadString(*source, "source"));
    }
    if (const JsonValue* coordinate = root.Find("coordinateSystem"); coordinate != nullptr) {
        PARCEL_TRY_ASSIGN(sidecar.coordinate_system, ReadString(*coordinate, "coordinateSystem"));
    }
    if (const JsonValue* settings = root.Find("settings"); settings != nullptr) {
        PARCEL_TRY(ParseSettings(*settings, sidecar.settings));
    }
    if (const JsonValue* cameras = root.Find("cameras"); cameras != nullptr) {
        if (!cameras->IsArray()) {
            return Fail("cameras", "expected an array");
        }
        sidecar.cameras.reserve(cameras->array_value.size());
        for (std::size_t i = 0; i < cameras->array_value.size(); ++i) {
            PARCEL_TRY_DECL(SidecarCamera, camera, ParseCamera(cameras->array_value[i], i));
            sidecar.cameras.push_back(std::move(camera));
        }
    }
    const std::pair<std::string_view, std::vector<SidecarObject>*> arrays[] = {
        {"lights", &sidecar.lights},
        {"decals", &sidecar.decals},
        {"reflectionProbes", &sidecar.reflection_probes},
        {"probes", &sidecar.reflection_probes},
        {"localFogVolumes", &sidecar.local_fog_volumes},
    };
    for (const auto& [key, target] : arrays) {
        const JsonValue* field = root.Find(key);
        if (field == nullptr) {
            continue;
        }
        PARCEL_TRY_DECL(std::vector<SidecarObject>, objects, ParseObjects(*field, key));
        target->insert(target->end(), std::make_move_iterator(objects.begin()),
                       std::make_move_iterator(objects.end()));
    }
    return sidecar;
}

Result<SceneSidecar> LoadSceneSidecarFile(std::string_view path) {
    core::PlatformFile platform;
    auto file = platform.OpenRead(path);
    if (!file) {
        return std::move(file).TakeError();
    }
    auto bytes = core::ReadAll(*file.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.Value().data()),
                                bytes.Value().size());
    auto sidecar = ParseSceneSidecar(text);
    if (!sidecar) {
        return std::move(sidecar).TakeError().WithPath(std::string(path));
    }
    return sidecar;
}

} // namespace parcel::assets
