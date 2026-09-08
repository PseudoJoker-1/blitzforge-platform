#pragma once

#include <stdint.h>

#include "../../include/wotb_mod_dava_native.h"

namespace wotbmod {
namespace v3 {

struct DavaNativeRegistry;

WotbModV3Result CreateDavaNativeRegistry(
    const WotbModDavaNativeBackend* backend,
    DavaNativeRegistry** out_registry);

void DestroyDavaNativeRegistry(DavaNativeRegistry* registry);

uint64_t DavaNativeRegistryCapabilities(
    const DavaNativeRegistry* registry);

WotbModV3Result DavaNativeYamlParseFile(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_document);

WotbModV3Result DavaNativeYamlExportUtf8(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken document,
    WotbModDavaNativeBuffer* inout_utf8_yaml);

WotbModV3Result DavaNativeArchiveOpenFile(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_archive);

WotbModV3Result DavaNativeArchiveGetEntryCount(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t* out_count);

WotbModV3Result DavaNativeArchiveGetEntry(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry);

WotbModV3Result DavaNativeArchiveReadEntry(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inout_data);

WotbModV3Result DavaNativeMaterialMutate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation);

WotbModV3Result DavaNativeMeshHotSwap(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacement_mesh);

WotbModV3Result DavaNativeTracerCreate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* out_tracer);

WotbModV3Result DavaNativeClassIsRegistered(
    DavaNativeRegistry* registry,
    const char* class_name,
    uint32_t* out_registered);

WotbModV3Result DavaNativeClassCreate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* out_object);

/*
 * The material of a node in the LIVE scene, as an NMaterial token.
 *
 * Mints under the NMATERIAL capability because that is what it hands back. The
 * provider retains the material before the token exists, so releasing the token
 * balances what the provider added rather than a count the mod never took.
 */
WotbModV3Result DavaNativeSceneMaterial(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object);

/*
 * The render batch of a node in the live scene, as an OBJECT_MESH_CONSUMER
 * token `mesh_hot_swap` accepts directly. The provider retains the batch
 * before the token exists; resolve-and-release within one operation is the
 * supported usage, exactly like the material route.
 */
WotbModV3Result DavaNativeSceneBatch(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object);

/* Every node name under the active scene, newline separated - discovery. */
WotbModV3Result DavaNativeSceneNodes(
    DavaNativeRegistry* registry,
    char* buffer,
    uint32_t* inout_size);

WotbModV3Result DavaNativeRelease(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken token);

uint32_t DavaNativeReleaseOwner(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner);

/* Process-wide loader installation. Each operation keeps the selected
 * registry alive for the complete callback, so replacement/removal can run
 * concurrently without retaining a stale provider table. */
WotbModV3Result InstallDavaNativeBackend(
    const WotbModDavaNativeBackend* backend);

void RemoveDavaNativeBackend();

uint64_t InstalledDavaNativeCapabilities();

WotbModV3Result InstalledDavaNativeYamlParseFile(
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_document);

WotbModV3Result InstalledDavaNativeYamlExportUtf8(
    WotbModV3Handle owner,
    WotbModDavaNativeToken document,
    WotbModDavaNativeBuffer* inout_utf8_yaml);

WotbModV3Result InstalledDavaNativeArchiveOpenFile(
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_archive);

WotbModV3Result InstalledDavaNativeArchiveGetEntryCount(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t* out_count);

WotbModV3Result InstalledDavaNativeArchiveGetEntry(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry);

WotbModV3Result InstalledDavaNativeArchiveReadEntry(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inout_data);

WotbModV3Result InstalledDavaNativeMaterialMutate(
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation);

WotbModV3Result InstalledDavaNativeMeshHotSwap(
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacement_mesh);

WotbModV3Result InstalledDavaNativeTracerCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* out_tracer);

WotbModV3Result InstalledDavaNativeClassIsRegistered(
    const char* class_name,
    uint32_t* out_registered);

WotbModV3Result InstalledDavaNativeClassCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* out_object);

WotbModV3Result InstalledDavaNativeSceneMaterial(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object);

WotbModV3Result InstalledDavaNativeSceneBatch(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object);

WotbModV3Result InstalledDavaNativeSceneNodes(
    char* buffer,
    uint32_t* inout_size);

WotbModV3Result InstalledDavaNativeRelease(
    WotbModV3Handle owner,
    WotbModDavaNativeToken token);

uint32_t InstalledDavaNativeReleaseOwner(WotbModV3Handle owner);

}  // namespace v3
}  // namespace wotbmod
