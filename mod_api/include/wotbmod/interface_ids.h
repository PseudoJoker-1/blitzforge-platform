#pragma once

#define WOTBMOD_V3_IFACE_CORE "wotbmod.core"
#define WOTBMOD_V3_IFACE_CAPABILITIES "wotbmod.capabilities"
#define WOTBMOD_V3_IFACE_PERMISSIONS "wotbmod.permissions"
#define WOTBMOD_V3_IFACE_HANDLES "wotbmod.handles"
#define WOTBMOD_V3_IFACE_LIFECYCLE "wotbmod.lifecycle"
#define WOTBMOD_V3_IFACE_HOOKS "wotbmod.hooks"
#define WOTBMOD_V3_IFACE_UNSAFE_NATIVE "wotbmod.unsafe.native"
#define WOTBMOD_V3_IFACE_EVENTS "wotbmod.events"
#define WOTBMOD_V3_IFACE_UI "wotbmod.ui"
#define WOTBMOD_V3_IFACE_SETTINGS "wotbmod.settings"
#define WOTBMOD_V3_IFACE_STORAGE "wotbmod.storage"
#define WOTBMOD_V3_IFACE_INPUT "wotbmod.input"
#define WOTBMOD_V3_IFACE_VFS "wotbmod.vfs"
#define WOTBMOD_V3_IFACE_RESOURCES "wotbmod.resources"
#define WOTBMOD_V3_IFACE_ASYNC "wotbmod.async"
#define WOTBMOD_V3_IFACE_HTTP "wotbmod.http"
#define WOTBMOD_V3_IFACE_INTERMOD "wotbmod.intermod"
#define WOTBMOD_V3_IFACE_RENDER "wotbmod.render"
#define WOTBMOD_V3_IFACE_RENDER_NATIVE "wotbmod.render.native"
#define WOTBMOD_V3_IFACE_CAMERA "wotbmod.camera"
#define WOTBMOD_V3_IFACE_SCENE "wotbmod.scene"
#define WOTBMOD_V3_IFACE_AUDIO "wotbmod.audio"
#define WOTBMOD_V3_IFACE_VEHICLE_VISUAL "wotbmod.vehicle.visual"
#define WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA "wotbmod.gameplay.camera"
#define WOTBMOD_V3_IFACE_GAMEPLAY_HUD "wotbmod.gameplay.hud"
#define WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR "wotbmod.gameplay.hangar"
#define WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY "wotbmod.gameplay.replay"
#define WOTBMOD_V3_IFACE_ENTITY_PUBLIC "wotbmod.entity.public"
#define WOTBMOD_V3_IFACE_BIGWORLD_RPC "wotbmod.bigworld.rpc"
#define WOTBMOD_V3_IFACE_PROJECTILE "wotbmod.projectile"
#define WOTBMOD_V3_IFACE_YAML "wotbmod.data.yaml"
#define WOTBMOD_V3_IFACE_ARCHIVE "wotbmod.archive"
#define WOTBMOD_V3_IFACE_LOADERS "wotbmod.loaders"
#define WOTBMOD_V3_IFACE_CLIENT "wotbmod.client"
#define WOTBMOD_V3_IFACE_DEVICE "wotbmod.client.device"
#define WOTBMOD_V3_IFACE_DIAGNOSTICS "wotbmod.diagnostics"
#define WOTBMOD_V3_IFACE_DEVTOOLS "wotbmod.devtools"
#define WOTBMOD_V3_IFACE_MANIFEST "wotbmod.manifest"
#define WOTBMOD_V3_IFACE_CATALOG "wotbmod.catalog"
#define WOTBMOD_V3_IFACE_CONTENT "wotbmod.content"

/*
 * Declared 2026-08-16, after RC1. Each of these is a NEW interface id backing
 * a NEW versioned table, never a widening of the table above it: the existing
 * ids already publish a capability status that describes a working subset, and
 * these five have no native backend yet. Sharing an id would force one status
 * to describe both, and every merge of "mostly works" with "does not exist"
 * lies about one of them. Separate ids also keep the grant narrow -- reading
 * one control's text should not require the whole wotbmod.ui surface.
 */
#define WOTBMOD_V3_IFACE_UI_READ "wotbmod.ui.read"
#define WOTBMOD_V3_IFACE_CAMERA_STATE "wotbmod.camera.state"
#define WOTBMOD_V3_IFACE_AUDIO_INTERCEPT "wotbmod.audio.intercept"
#define WOTBMOD_V3_IFACE_SCENE_ENUMERATE "wotbmod.scene.enumerate"
#define WOTBMOD_V3_IFACE_TRACER "wotbmod.tracer"
#define WOTBMOD_V3_IFACE_GES "wotbmod.ges"
/* API 1.1, 2026-09-08: login cluster switch of the current region. */
#define WOTBMOD_V3_IFACE_SESSION_CLUSTER "wotbmod.session.cluster"

#define WOTBMOD_V3_IFACE_VERSION_1 1u
#define WOTBMOD_V3_IFACE_VERSION_2 2u
#define WOTBMOD_V3_IFACE_VERSION_3 3u
