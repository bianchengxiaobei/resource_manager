#include "parcel/entt/scene_loader.hpp"

#include "parcel/core/error.hpp"

#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace parcel_entt {
namespace {

using parcel::core::ErrorCode;
using parcel::core::MakeError;
using parcel::scene::ComponentChunk;
using parcel::scene::EntityId;
using parcel::scene::SceneEntityRecord;
using parcel::scene::TransformComponent;

struct EnttSceneContext {
    ::entt::registry* registry = nullptr;
    std::vector<::entt::entity> entities;
};

void DestroyContext(EnttSceneContext& context) {
    if (context.registry == nullptr) {
        context.entities.clear();
        return;
    }
    for (auto it = context.entities.rbegin(); it != context.entities.rend(); ++it) {
        if (context.registry->valid(*it)) {
            context.registry->destroy(*it);
        }
    }
    context.entities.clear();
}

Result<void> ValidateSceneForEntt(const SceneData& scene, const EnttComponentRegistry& components) {
    std::unordered_map<EntityId, const SceneEntityRecord*> records;
    records.reserve(scene.entities.size());
    for (const SceneEntityRecord& entity : scene.entities) {
        if (entity.id.IsRoot()) {
            return MakeError(ErrorCode::Corrupt, "scene entity id 0 is reserved");
        }
        if (!records.emplace(entity.id.value, &entity).second) {
            return MakeError(ErrorCode::Corrupt, "duplicate scene entity id");
        }
    }

    for (const SceneEntityRecord& entity : scene.entities) {
        if (!entity.parent.IsRoot() && records.find(entity.parent.value) == records.end()) {
            return MakeError(ErrorCode::Corrupt, "scene entity parent does not exist");
        }
        if (entity.parent == entity.id) {
            return MakeError(ErrorCode::Corrupt, "scene entity cannot parent itself");
        }
    }

    for (const SceneEntityRecord& entity : scene.entities) {
        SceneEntityId current = entity.parent;
        std::size_t steps = 0;
        while (!current.IsRoot()) {
            const auto it = records.find(current.value);
            if (it == records.end()) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent chain is broken");
            }
            if (it->second->parent == entity.id) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent cycle");
            }
            current = it->second->parent;
            if (++steps > scene.entities.size()) {
                return MakeError(ErrorCode::Corrupt, "scene entity parent cycle");
            }
        }
    }

    std::unordered_set<ComponentTypeId> component_types;
    for (const ComponentChunk& chunk : scene.components) {
        const EnttComponentRegistry::Entry* entry = components.Find(chunk.type);
        if (entry == nullptr) {
            return MakeError(ErrorCode::Unsupported, "unknown component type id");
        }
        if (entry->schema_version != chunk.schema_version) {
            return MakeError(ErrorCode::Unsupported, "unsupported component schema version");
        }
        if (!component_types.insert(chunk.type).second) {
            return MakeError(ErrorCode::Corrupt, "duplicate component chunk type");
        }
        if (chunk.owners.size() != chunk.payloads.size()) {
            return MakeError(ErrorCode::Corrupt, "component owners and payloads differ");
        }

        std::unordered_set<EntityId> owners;
        owners.reserve(chunk.owners.size());
        for (std::size_t index = 0; index < chunk.owners.size(); ++index) {
            const SceneEntityId owner = chunk.owners[index];
            if (records.find(owner.value) == records.end()) {
                return MakeError(ErrorCode::Corrupt, "component owner does not exist");
            }
            if (!owners.insert(owner.value).second) {
                return MakeError(ErrorCode::Corrupt, "duplicate component owner");
            }
            if (chunk.payloads[index].empty()) {
                return MakeError(ErrorCode::Corrupt, "component payload is empty");
            }
        }

        if (chunk.type == parcel::scene::kTransformComponentType) {
            for (const std::vector<std::byte>& payload : chunk.payloads) {
                auto transform = parcel::scene::ComponentCodec<TransformComponent>::Decode(payload);
                if (!transform) {
                    return std::move(transform).TakeError();
                }
                if (!transform.Value().parent.IsRoot() &&
                    records.find(transform.Value().parent.value) == records.end()) {
                    return MakeError(ErrorCode::Corrupt, "transform parent entity does not exist");
                }
            }
        }
    }
    return {};
}

} // namespace

Result<void> EnttComponentRegistry::Register(ComponentTypeId id, std::string_view name,
                                             std::uint32_t schema_version, EmplaceFn emplace) {
    if (id == 0 || name.empty() || schema_version == 0 || emplace == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "entt component type info is invalid");
    }
    for (const Entry& existing : types_) {
        if (existing.id == id) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate entt component type id");
        }
        if (existing.name == name) {
            return MakeError(ErrorCode::InvalidArgument, "duplicate entt component type name");
        }
    }
    types_.push_back(Entry{id, std::string(name), schema_version, emplace});
    return {};
}

const EnttComponentRegistry::Entry* EnttComponentRegistry::Find(ComponentTypeId id) const noexcept {
    for (const Entry& entry : types_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const EnttComponentRegistry::Entry*
EnttComponentRegistry::Find(std::string_view name) const noexcept {
    for (const Entry& entry : types_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

Result<void> EnttComponentRegistry::Emplace(::entt::registry& registry, ::entt::entity entity,
                                            ComponentTypeId type, std::uint32_t schema_version,
                                            std::span<const std::byte> payload) const {
    const Entry* entry = Find(type);
    if (entry == nullptr) {
        return MakeError(ErrorCode::Unsupported, "unknown component type id");
    }
    if (entry->schema_version != schema_version) {
        return MakeError(ErrorCode::Unsupported, "unsupported component schema version");
    }
    if (!registry.valid(entity)) {
        return MakeError(ErrorCode::InvalidArgument, "component owner entity is invalid");
    }
    return entry->emplace(registry, entity, payload);
}

const EnttComponentRegistry& StandardEnttComponentRegistry() {
    static const EnttComponentRegistry registry = [] {
        EnttComponentRegistry result;
        const bool registered = result.Register<parcel::scene::TransformComponent>() &&
                                result.Register<parcel::scene::CameraComponent>() &&
                                result.Register<parcel::scene::LightComponent>() &&
                                result.Register<parcel::scene::MeshRendererComponent>() &&
                                result.Register<parcel::scene::AnimationPlayerComponent>() &&
                                result.Register<parcel::scene::SkinComponent>() &&
                                result.Register<parcel::scene::MorphComponent>() &&
                                result.Register<parcel::scene::DecalComponent>() &&
                                result.Register<parcel::scene::ReflectionProbeComponent>() &&
                                result.Register<parcel::scene::LocalFogVolumeComponent>() &&
                                result.Register<parcel::scene::AudioSourceComponent>() &&
                                result.Register<parcel::scene::AudioSourceMixComponent>() &&
                                result.Register<parcel::scene::AudioReverbZoneComponent>() &&
                                result.Register<parcel::scene::AudioListenerComponent>() &&
                                result.Register<parcel::scene::RigidBodyComponent>() &&
                                result.Register<parcel::scene::ColliderComponent>() &&
                                result.Register<parcel::scene::ScriptComponent>();
        PARCEL_VERIFY(registered);
        return result;
    }();
    return registry;
}

const EnttComponentRegistry& EnttSceneLoader::Components() const noexcept {
    return components_ == nullptr ? StandardEnttComponentRegistry() : *components_;
}

Result<parcel::scene::SceneInstance> EnttSceneLoader::Instantiate(const SceneData& scene,
                                                                  void* registry_pointer) {
    if (registry_pointer == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "entt registry is null");
    }

    auto* registry = static_cast<::entt::registry*>(registry_pointer);
    const EnttComponentRegistry& components = Components();
    PARCEL_TRY(ValidateSceneForEntt(scene, components));

    auto context = std::make_shared<EnttSceneContext>();
    context->registry = registry;
    context->entities.reserve(scene.entities.size());

    std::unordered_map<EntityId, ::entt::entity> lookup;
    lookup.reserve(scene.entities.size());

    for (std::size_t entity_index = 0; entity_index < scene.entities.size(); ++entity_index) {
        const SceneEntityRecord& record = scene.entities[entity_index];
        const ::entt::entity entity = registry->create();
        if (entity == ::entt::null) {
            DestroyContext(*context);
            return MakeError(ErrorCode::Internal, "entt registry failed to create entity");
        }
        context->entities.push_back(entity);
        lookup.emplace(record.id.value, entity);

        registry->emplace<SceneEntityComponent>(
            entity, SceneEntityComponent{record.id, record.name, record.layer, record.flags,
                                         static_cast<std::uint32_t>(entity_index)});

        TransformComponent transform;
        transform.position = record.transform.position;
        transform.rotation = record.transform.rotation;
        transform.scale = record.transform.scale;
        transform.parent = record.parent;
        registry->emplace<TransformComponent>(entity, std::move(transform));
    }

    for (const SceneEntityRecord& record : scene.entities) {
        if (record.parent.IsRoot()) {
            continue;
        }
        const auto parent = lookup.find(record.parent.value);
        if (parent == lookup.end()) {
            DestroyContext(*context);
            return MakeError(ErrorCode::Corrupt, "scene entity parent does not exist");
        }
        registry->emplace<ParentEntityComponent>(
            lookup.at(record.id.value), ParentEntityComponent{record.parent, parent->second});
    }

    for (const ComponentChunk& chunk : scene.components) {
        for (std::size_t index = 0; index < chunk.owners.size(); ++index) {
            const SceneEntityId owner = chunk.owners[index];
            const auto entity_it = lookup.find(owner.value);
            if (entity_it == lookup.end()) {
                DestroyContext(*context);
                return MakeError(ErrorCode::Corrupt, "component owner does not exist");
            }
            auto emplaced = components.Emplace(*registry, entity_it->second, chunk.type,
                                               chunk.schema_version, chunk.payloads[index]);
            if (!emplaced) {
                DestroyContext(*context);
                return std::move(emplaced).TakeError();
            }
            if (chunk.type == parcel::scene::kTransformComponentType) {
                if (TransformComponent* transform =
                        registry->try_get<TransformComponent>(entity_it->second)) {
                    transform->parent = scene.FindEntity(owner)->parent;
                }
            }
        }
    }

    parcel::scene::SceneInstance instance;
    instance.registry = registry_pointer;
    instance.entities.reserve(scene.entities.size());
    for (const SceneEntityRecord& record : scene.entities) {
        instance.entities.push_back(record.id);
    }
    instance.context = std::move(context);
    return instance;
}

Result<void> EnttSceneLoader::Destroy(parcel::scene::SceneInstance& instance) {
    if (!instance.context) {
        if (!instance.entities.empty()) {
            return MakeError(ErrorCode::InvalidArgument,
                             "scene instance has no entt ownership context");
        }
        instance.registry = nullptr;
        return {};
    }

    auto context = std::static_pointer_cast<EnttSceneContext>(instance.context);
    if (context->registry == nullptr ||
        (instance.registry != nullptr && instance.registry != context->registry)) {
        return MakeError(ErrorCode::InvalidArgument, "scene instance registry does not match");
    }

    DestroyContext(*context);
    instance.entities.clear();
    instance.context.reset();
    instance.registry = nullptr;
    return {};
}

} // namespace parcel_entt
