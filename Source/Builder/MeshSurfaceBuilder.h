// SPDX-License-Identifier: MIT
#pragma once
#include <AzCore/Component/Component.h>
#include <AzCore/std/parallel/atomic.h>
#include <AssetBuilderSDK/AssetBuilderSDK.h>

namespace SilPOM
{
class MeshSurfaceBuilder final : public AZ::Component, private AssetBuilderSDK::AssetBuilderCommandBus::Handler
{
public:
    AZ_COMPONENT(MeshSurfaceBuilder, "{97AE18AE-358E-4B58-B019-1B122EC3BFCC}");
    static void Reflect(AZ::ReflectContext* context);
    void Activate() override;
    void Deactivate() override;
    void ShutDown() override { m_stopping = true; }
    void CreateJobs(const AssetBuilderSDK::CreateJobsRequest&, AssetBuilderSDK::CreateJobsResponse&);
    void ProcessJob(const AssetBuilderSDK::ProcessJobRequest&, AssetBuilderSDK::ProcessJobResponse&);
private:
    AZStd::atomic_bool m_stopping{false};
};
}
