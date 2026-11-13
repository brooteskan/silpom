// SPDX-License-Identifier: MIT
#include "LearnedMaterialBuilder.h"
#include <SilPOM/LearnedMaterialAsset.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/Serialization/EditContextConstants.inl>
#include <AzCore/Serialization/Utils.h>
#include <AzCore/Utils/Utils.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/std/functional.h>
#include <openssl/sha.h>
namespace SilPOM
{
void LearnedMaterialBuilder::Reflect(AZ::ReflectContext* c)
{
    if(auto* sc=azrtti_cast<AZ::SerializeContext*>(c))sc->Class<LearnedMaterialBuilder,AZ::Component>()->Version(1)
        ->Attribute(AZ::Edit::Attributes::SystemComponentTags,AZStd::vector<AZ::Crc32>{AssetBuilderSDK::ComponentTags::AssetBuilder});
}
void LearnedMaterialBuilder::Activate()
{
    AssetBuilderSDK::AssetBuilderDesc d;d.m_name="SilPOM learned material";d.m_version=3;
    d.m_analysisFingerprint="material-v3-level4-and-complete-appearance-mips";d.m_busId=azrtti_typeid<LearnedMaterialBuilder>();
    d.m_patterns.emplace_back("*.l4mat",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_patterns.emplace_back("*.l5app",AssetBuilderSDK::AssetBuilderPattern::PatternType::Wildcard);
    d.m_createJobFunction=AZStd::bind(&LearnedMaterialBuilder::CreateJobs,this,AZStd::placeholders::_1,AZStd::placeholders::_2);
    d.m_processJobFunction=AZStd::bind(&LearnedMaterialBuilder::ProcessJob,this,AZStd::placeholders::_1,AZStd::placeholders::_2);
    BusConnect(d.m_busId);AssetBuilderSDK::AssetBuilderBus::Broadcast(&AssetBuilderSDK::AssetBuilderBusTraits::RegisterBuilderInformation,d);
}
void LearnedMaterialBuilder::Deactivate(){BusDisconnect();}
void LearnedMaterialBuilder::CreateJobs(const AssetBuilderSDK::CreateJobsRequest& q,AssetBuilderSDK::CreateJobsResponse& r)
{
    for(const char* file:{"basecolor.png","normal.png","roughness.png"})
        r.m_sourceFileDependencyList.emplace_back((AZ::IO::Path(AZ::Utils::GetProjectPath())/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall"/file).String(),AZ::Uuid::CreateNull());
    if(AZ::IO::Path(q.m_sourceFile).Extension()==".l5app")
    {
        r.m_sourceFileDependencyList.emplace_back((AZ::IO::Path(AZ::Utils::GetProjectPath())/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall/height.png").String(),AZ::Uuid::CreateNull());
        r.m_sourceFileDependencyList.emplace_back((AZ::IO::Path(AZ::Utils::GetProjectPath())/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/tower-open-shaft-no-landings-silpom.silpom.json").String(),AZ::Uuid::CreateNull());
    }
    for(const auto& p:q.m_enabledPlatforms){AssetBuilderSDK::JobDescriptor j;j.m_jobKey="SilPOM learned material";j.SetPlatformIdentifier(p.m_identifier.c_str());j.m_failOnError=true;r.m_createJobOutputs.push_back(j);}
    r.m_result=AssetBuilderSDK::CreateJobsResultCode::Success;
}
void LearnedMaterialBuilder::ProcessJob(const AssetBuilderSDK::ProcessJobRequest& q,AssetBuilderSDK::ProcessJobResponse& r)
{
    r.m_resultCode=AssetBuilderSDK::ProcessJobResult_Failed;
    auto bytes=AZ::Utils::ReadFile(q.m_fullPath);if(!bytes.IsSuccess())return;
    LearnedMaterialAsset asset;asset.m_bytes.assign(bytes.GetValue().begin(),bytes.GetValue().end());
    if(!asset.Validate()){AZ_Error("SilPOM",false,"Invalid learned material bundle");return;}
    AZStd::string original;
    for(const char* file:{"basecolor.png","normal.png","roughness.png"})
    {
        auto data=AZ::Utils::ReadFile((AZ::IO::Path(AZ::Utils::GetProjectPath())/"Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall"/file).String());
        if(!data.IsSuccess()){AZ_Error("SilPOM",false,"Learned wall material source is unavailable");return;}
        original+=data.GetValue();
    }
    if(!memcmp(asset.Header().magic,"SILPAPP5",8))
    {
        for(const char* file:{"Assets/SilPOMPlanarAB/IrishFieldstoneTowerWelded/wall/height.png",
            "Assets/SilPOMPlanarAB/IrishFieldstoneTowerOpenShaft/tower-open-shaft-no-landings-silpom.silpom.json"})
        {
            auto data=AZ::Utils::ReadFile((AZ::IO::Path(AZ::Utils::GetProjectPath())/file).String());
            if(!data.IsSuccess()){AZ_Error("SilPOM",false,"Level 5 training source is unavailable");return;}
            original+=data.GetValue();
        }
    }
    unsigned char sourceHash[32];SHA256(reinterpret_cast<const unsigned char*>(original.data()),original.size(),sourceHash);
    if(memcmp(sourceHash,asset.Header().sourceHash,32)){AZ_Error("SilPOM",false,"Wall material source changed; retrain the Level 4 bundle");return;}
    unsigned char hash[32];SHA256(asset.m_bytes.data(),asset.m_bytes.size(),hash);
    for(auto b:hash)asset.m_generation+=AZStd::string::format("%02x",b);
    const auto output=(AZ::IO::Path(q.m_tempDirPath)/(AZ::IO::Path(q.m_sourceFile).Stem().String()+".silpommaterial")).String();
    if(!AZ::Utils::SaveObjectToFile(output,AZ::DataStream::ST_BINARY,&asset))return;
    AssetBuilderSDK::JobProduct p(output,azrtti_typeid<LearnedMaterialAsset>(),1);p.m_dependenciesHandled=true;
    r.m_outputProducts.push_back(p);r.m_resultCode=AssetBuilderSDK::ProcessJobResult_Success;
}
}
