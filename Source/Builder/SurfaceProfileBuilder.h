// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AssetBuilderSDK/AssetBuilderSDK.h>
namespace SilPOM
{
// .silpomprofile + .silpomweights.json -> .silpomprofileasset. Runs
// Tools/silpom_profile.py to produce missing weights.
class SurfaceProfileBuilder final : public AZ::Component, private AssetBuilderSDK::AssetBuilderCommandBus::Handler
{
public:
    AZ_COMPONENT(SurfaceProfileBuilder, "{2D4B8E0B-7E71-4A5F-9C66-0D1E4E6A93B2}");
    static void Reflect(AZ::ReflectContext*);
    void Activate() override;
    void Deactivate() override;
    void ShutDown() override {}
    void CreateJobs(const AssetBuilderSDK::CreateJobsRequest&, AssetBuilderSDK::CreateJobsResponse&);
    void ProcessJob(const AssetBuilderSDK::ProcessJobRequest&, AssetBuilderSDK::ProcessJobResponse&);
};
} // namespace SilPOM
