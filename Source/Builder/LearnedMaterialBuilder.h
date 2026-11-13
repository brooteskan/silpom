// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AssetBuilderSDK/AssetBuilderSDK.h>
namespace SilPOM
{
class LearnedMaterialBuilder final : public AZ::Component,private AssetBuilderSDK::AssetBuilderCommandBus::Handler
{
public:
    AZ_COMPONENT(LearnedMaterialBuilder,"{0ED9D510-CA8B-444A-84A5-07DF7BCA711A}");
    static void Reflect(AZ::ReflectContext*);
    void Activate() override;void Deactivate() override;void ShutDown() override {}
    void CreateJobs(const AssetBuilderSDK::CreateJobsRequest&,AssetBuilderSDK::CreateJobsResponse&);
    void ProcessJob(const AssetBuilderSDK::ProcessJobRequest&,AssetBuilderSDK::ProcessJobResponse&);
};
}
