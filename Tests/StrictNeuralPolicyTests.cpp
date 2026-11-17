// SPDX-License-Identifier: MIT
#include "../Include/SilPOM/StrictNeuralPolicy.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace SilPOM::StrictNeuralPolicy;
namespace
{
unsigned checks=0;
void Require(bool condition,const char* message)
{ ++checks;if(!condition)throw std::runtime_error(message); }
Capability Product(std::string_view tag,std::string_view suffix,std::string_view variant="")
{
    return Classify(tag,"materials/types/silpommesh/silpommesh_mainpipeline_"+std::string(suffix),variant);
}
PacketProof Pair()
{
    PacketProof proof;
    proof.Add(Product("silpom_hit_forward_neural_only","silpomhitforwardneuralonly.azshader"));
    proof.Add(Product("silpom_hit_depth_lod_near","silpomhitdepthlodnear.azshader"));
    return proof;
}
void CheckSceneSubscriptionLifecycle()
{
    // Model the engine's actual collection/submission boundary: collected
    // lists retain the native draw item, and submission reads instanceCount
    // rather than rechecking the enabled flag. A per-mesh option rebuild can
    // replace that item without delivering the packet-update notification.
    struct NativeDraw { bool enabled=true;unsigned instanceCount=1; };
    struct Controller
    {
        SceneDuties duties;
        NativeDraw packet;
        bool sceneAvailable=true,subscribed=false,certified=false,resolvePublished=true;
        void UpdateSubscription() { subscribed=sceneAvailable&&duties.NeedsNotifications(); }
        void FinalRenderBarrier()
        {
            if(!subscribed)return;
            const bool enable=CanEnable(true,duties.requestedStrict,certified);
            packet.enabled=enable;packet.instanceCount=enable?1u:0u;resolvePublished=enable;
        }
        void UnnotifiedRebuild() { packet={true,1}; }
    } controller;

    // Publish a nontrained, non-strict generation. It has no trained rendering
    // duties, so the ordinary scene handler is initially disconnected.
    controller.UpdateSubscription();
    Require(!controller.subscribed,"An ordinary nontrained generation acquired unneeded scene callbacks");
    controller.duties.requestedStrict=true;
    controller.UpdateSubscription(); // Must precede validation/loading returns.
    Require(controller.subscribed&&!controller.duties.TrainedRendering(),
        "A strict request without trained profiles lost its final render callback");
    controller.duties.activeStrict=true;
    controller.packet.enabled=false;
    controller.resolvePublished=false;
    // Validation rejects the strict request and no candidate is constructed.
    // Repeated rejected/loading frames must retain the subscription regardless.
    for(unsigned frame=0;frame<4;++frame)
    {
        controller.UpdateSubscription();
        controller.UnnotifiedRebuild();
        NativeDraw* collected=&controller.packet;
        Require(collected->enabled&&collected->instanceCount==1,
            "Regression did not recreate Atom's default-enabled rebuilt packet");
        controller.FinalRenderBarrier();
        Require(collected->instanceCount==0&&!controller.resolvePublished,
            "Rejected strict request allowed a rebuilt native draw or resolve publication");
    }

    // A compatible generation may recover while strict policy remains active;
    // certification loss in a later unnotified rebuild must suppress it again.
    controller.certified=Pair().Complete();
    controller.UnnotifiedRebuild();controller.FinalRenderBarrier();
    Require(controller.packet.instanceCount==1&&controller.resolvePublished&&controller.subscribed,
        "Compatible certification did not restore rendering under the persistent strict subscription");
    controller.certified=false;
    controller.UnnotifiedRebuild();controller.FinalRenderBarrier();
    Require(controller.packet.instanceCount==0&&!controller.resolvePublished,
        "A later shader rebuild escaped the retained strict subscription");

    // Explicit exit keeps a restoration duty until the old generation's draw
    // counts and resolve state have actually been restored.
    controller.duties.requestedStrict=false;
    controller.UpdateSubscription();
    Require(controller.subscribed,"Strict exit disconnected before restoring the suppressed generation");
    controller.FinalRenderBarrier();
    Require(controller.packet.instanceCount==1&&controller.resolvePublished,
        "Explicit strict exit left native counts or resolve publication suppressed");
    controller.duties.activeStrict=false;
    controller.UpdateSubscription();
    Require(!controller.subscribed,"Finished nontrained restoration retained unneeded scene callbacks");

    // Scene creation can lag the request. Tick/Prepare retries must acquire the
    // same strict duty without needing validation to reach generation setup.
    controller.sceneAvailable=false;controller.duties.requestedStrict=true;
    controller.UpdateSubscription();
    Require(!controller.subscribed,"A missing scene appeared subscribed");
    controller.sceneAvailable=true;controller.UpdateSubscription();
    controller.UnnotifiedRebuild();controller.FinalRenderBarrier();
    Require(controller.subscribed&&controller.packet.instanceCount==0&&!controller.resolvePublished,
        "Scene creation after rejected strict validation did not recover the final barrier");

    constexpr bool SceneDuties::* independentDuties[]={
        &SceneDuties::requestedStrict,&SceneDuties::requestedTrained,&SceneDuties::activeStrict,
        &SceneDuties::pendingStrict,&SceneDuties::activeRestoration,&SceneDuties::pendingRestoration,
        &SceneDuties::activeTrained,&SceneDuties::pendingTrained};
    for(auto member:independentDuties)
    {
        SceneDuties duty;duty.*member=true;
        Require(duty.NeedsNotifications(),"One active/pending/requested rendering duty could lose its callback");
    }
}
}
int main()
{
    try
    {
        CheckSceneSubscriptionLifecycle();
        Require(Pair().Complete(),"Built-in strict lighting plus trained no-march depth was rejected");
        const std::array<std::pair<const char*,const char*>,6> strictForward{{
            {"silpom_hit_forward_neural_only","silpomhitforwardneuralonly.azshader"},
            {"silpom_hit_forward_shared","silpomhitforwardshared.azshader"},
            {"silpom_hit_forward_shared_proxy","silpomhitforwardsharedproxy.azshader"},
            {"silpom_hit_forward_level_five","silpomhitforwardlevelfive.azshader"},
            {"silpom_hit_forward_shared_compare","silpomhitforwardsharedcompare.azshader"},
            {"silpom_hit_forward_angular_proxy_ambient","silpomhitforwardangularproxyambient.azshader"}}};
        for(const auto& [tag,suffix]:strictForward)
        {
            for(const auto variant:{"","NoMSAA"})
                Require(Product(tag,suffix,variant)==Capability::Visibility,"Supported strict forward variant was not certified");
            Require(Product(tag,suffix,"Custom")==Capability::Unsupported,"An unknown supervariant was trusted");
            Require(Classify(tag,"custom/neural-only.azshader","")==Capability::Unsupported,"A logical tag certified an override");
            Require(Product(tag,"silpomhitforward.azshader")==Capability::Unsupported,"A strict tag disguised the hybrid product");
            Require(Classify(tag,"materials/types/silpommesh/silpommeshuv_mainpipeline_"+std::string(suffix),"")==Capability::Visibility,
                "UV material's unchanged strict renderer was rejected");
        }
        for(const auto& [tag,suffix]:std::array<std::pair<const char*,const char*>,4>{{
            {"silpom_hit_forward","silpomhitforward.azshader"},
            {"silpom_hit_depth","silpomhitdepth.azshader"},
            {"silpom_hit_depth_bounds","silpomhitdepthbounds.azshader"},
            {"silpom_hit_depth_neural","silpomhitdepthneural.azshader"}}})
        {
            Require(Product(tag,suffix)==Capability::Unsupported,"A marcher-capable generation was certified");
            auto proof=Pair();proof.Add(Product(tag,suffix));
            Require(!proof.Complete(),"One unsafe shader escaped an otherwise valid generation proof");
        }
        PacketProof lightingOnly;
        lightingOnly.Add(Product("silpom_hit_forward_neural_only","silpomhitforwardneuralonly.azshader"));
        Require(!lightingOnly.Complete(),"Strict lighting was mistaken for a pipeline-wide no-march proof");
        lightingOnly.Add(Product("silpom_hit_depth_lod_shell","silpomhitdepthlodshell.azshader"));
        Require(!lightingOnly.Complete(),"Analytic recovery substituted for the trained primary depth path");
        lightingOnly.Add(Product("silpom_hit_depth_neural_cache","silpomhitdepthneuralcache.azshader"));
        Require(lightingOnly.Complete(),"Trained depth with analytic recovery failed certification");

        // Policy changes affect an already published generation immediately;
        // loading/validation failures never authorize an incompatible draw.
        bool published=true,strict=false,certified=false;
        Require(CanEnable(published,strict,certified),"Non-strict existing generation was suppressed");
        strict=true;
        Require(!CanEnable(published,strict,certified),"Requesting strict mode retained an old marching generation");
        for(unsigned loadingFailures=0;loadingFailures<4;++loadingFailures)
            Require(!CanEnable(published,strict,certified),"A replacement failure re-enabled a marcher");
        certified=Pair().Complete();
        Require(CanEnable(published,strict,certified),"A certified prior generation could not remain visible");
        auto changed=Pair();changed.Add(Capability::Unsupported);certified=changed.Complete();
        Require(!CanEnable(published,strict,certified),"A packet/product change retained stale certification");
        certified=Pair().Complete();
        Require(CanEnable(published,strict,certified),"A valid shader replacement could not recover");
        published=false;
        Require(!CanEnable(published,strict,certified),"A partial new generation became visible after certification");
        strict=false;certified=false;
        Require(!CanEnable(published,strict,certified),"Leaving strict mode published an incomplete generation");
        std::cout<<checks<<" strict neural policy checks passed: actual products, trained depth, publication and recovery\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
