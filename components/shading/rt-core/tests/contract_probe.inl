// Independent, analytic contracts for material transport and reconstruction.
// These checks cannot approve appearance; they reject wrong cross-stage data.
static bool ContractCheck(rtc::PathView view,const std::string &out) {
    const float positions[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1};
    const float uv[]={0,0,1,0,1,1,0,1},normal[]={0,0,-1,0,0,-1,0,0,-1,0,0,-1};
    const float tangent[]={0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1};
    const uint32_t indices[]={0,2,1,0,3,2},backIndices[]={0,1,2,0,2,3};
    auto vb=Data(positions,sizeof positions),ib=Data(indices,sizeof indices),back=Data(backIndices,sizeof backIndices);
    auto uvb=Data(uv,sizeof uv),nb=Data(normal,sizeof normal),tb=Data(tangent,sizeof tangent);
    rtc::MaterialDesc solid,transparent;transparent.kind=rtc::MaterialKind::Translucent;
    rtc::Submesh sub[2];for(auto &s:sub)s.count=6;
    sub[0].material=rtc::CreateMaterial(solid);sub[1].material=rtc::CreateMaterial(transparent);
    rtc::Geometry geo[7];
    for(unsigned i=0;i<7;i++) {
        auto &g=geo[i];g.id=300+i;g.flags=rtc::kRigid;g.vertexCount=4;
        g.position={vb.Get(),0,12,rtc::VertexFormat::Float3};g.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
        g.indexBuffer=ib.Get();g.index32=1;g.submeshCount=1;g.submeshes=&sub[i?1:0];g.matrix[11]=-.1f*i;
    }
    rtc::Frame frame;frame.frame=300;frame.completedFrame=299;
    frame.geometries=geo;frame.geometryCount=2;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    view.samples=1;view.maxBounces=1;view.motion=nullptr;view.detailReference=nullptr;view.jitter[0]=view.jitter[1]=0;
    rtc::PathMaterial opaque,layer;opaque.flags=1024;for(auto &v:opaque.emissionColor)v=1;
    const rtc::PathResponsivity *responseTest=nullptr;
    auto render=[&]() {
        if(!rtc::SetPathMaterial(sub[0].material,opaque)||!rtc::SetPathMaterial(sub[1].material,layer))return false;
        Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathTrace(G.list.Get(),view,responseTest))return false;
        SubmitWait();Drain("PT contracts");frame.completedFrame=frame.frame++;return true;
    };
    const size_t center=(64*128+64)*4;
    auto scalar=[&](ID3D12Resource *texture,size_t at) {return Half(ReadTexture(texture,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4)[at]);};
    bool pass=true;std::string json="{";bool first=true;
    auto result=[&](const char *name,bool ok,float value) {
        char text[192];snprintf(text,sizeof text,"%s\"%s\":{\"pass\":%s,\"value\":%.8g}",first?"":",",name,ok?"true":"false",value);
        json+=text;first=false;pass&=ok;
    };
    // Coverage below one half never becomes the primary guide, for ANY seed.
    const float alpha49[]={1,1,1,.49f};auto alpha=FixtureTexture(1,alpha49);
    layer.alpha=alpha.Get();bool stable=true;
    for(unsigned seed:{7u,43u,92u}) {
        view.seed=seed;if(!render())return false;
        stable&=scalar(view.diffuse,center+3)==1;
    }
    result("deterministic_coverage",stable,stable?1.f:0.f);
    // An alpha darkening overlay attenuates underlying radiance without
    // becoming a black opaque guide. The result is exactly 1 - alpha.
    layer=rtc::PathMaterial{};layer.flags=2048|1048576;layer.baseColor[3]=.25f;layer.transmission=1;
    if(!render())return false;float attenuation=scalar(view.color,center);
    result("attenuation_layer",std::abs(attenuation-.75f)<.002f,attenuation);
    // Additive emission must preserve the entire background, unlike coverage.
    layer=rtc::PathMaterial{};layer.flags=1024|2048|2097152;layer.transmission=1;
    for(auto &v:layer.emissionColor)v=.2f;
    if(!render())return false;float additive=scalar(view.color,center);
    result("additive_layer",std::abs(additive-1.2f)<.003f,additive);
    // Six thin sheets must not exhaust a one-scattering-vertex budget. No
    // object-specific flags: the transparent domain alone selects PSR.
    layer=rtc::PathMaterial{};layer.flags=2048;layer.transmission=1;layer.roughness=.25f;
    frame.geometryCount=7;view.samples=4;if(!render())return false;
    auto through=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);double mean=0;
    for(size_t i=0;i<through.size();i+=4)mean+=Half(through[i])/(128.*128.);
    result("six_sheet_transport",mean>.65 && mean<.9 && scalar(view.diffuse,center+3)==1,(float)mean);
    // Authored tangent rotates the normal-map x axis into world +Y. Triangle
    // UV reconstruction would incorrectly leave it pointing along world +X.
    frame.geometryCount=1;view.samples=1;opaque=rtc::PathMaterial{};
    const float bump[]={.8f,.5f,1,1};auto bumpTexture=FixtureTexture(1,bump);
    opaque.normal=bumpTexture.Get();opaque.flags=4;
    geo[0].normal={nb.Get(),0,12,rtc::VertexFormat::Float3};geo[0].tangent={tb.Get(),0,16,rtc::VertexFormat::Float4};
    if(!render())return false;
    auto guide=ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    result("authored_tangent",std::abs(Half(guide[center]))<.002f && std::abs(Half(guide[center+1])-.6f)<.002f,Half(guide[center+1]));
    // A camera respects back-face culling, independently from shadow transport.
    opaque=rtc::PathMaterial{};opaque.flags=1024;opaque.emissionColor[0]=1;
    geo[0].flags|=rtc::kPrimaryCullBack;if(!render())return false;float front=scalar(view.color,center);
    geo[0].indexBuffer=back.Get();if(!render())return false;float reverse=scalar(view.color,center);
    result("primary_backface_cull",front==1 && reverse==0,front-reverse);
    geo[0].flags=rtc::kRigid|rtc::kPrimaryCullFront;
    if(!render())return false;float frontCulledReverse=scalar(view.color,center);
    geo[0].indexBuffer=ib.Get();if(!render())return false;float frontCulled=scalar(view.color,center);
    result("primary_frontface_cull",frontCulledReverse==1 && frontCulled==0,frontCulledReverse-frontCulled);
    geo[0].flags=rtc::kRigid|rtc::kPrimaryCullBack;geo[0].matrix[0]=-1;
    if(!render())return false;float mirroredFront=scalar(view.color,center);
    geo[0].indexBuffer=back.Get();if(!render())return false;float mirroredBack=scalar(view.color,center);
    result("primary_mirrored_cull",mirroredFront==1 && mirroredBack==0,mirroredFront-mirroredBack);
    geo[0].matrix[0]=1;
    geo[0].indexBuffer=ib.Get();geo[0].flags=rtc::kRigid;
    // Clamp must not repeat an emissive image outside its UV range.
    const float edge[]={1,1,1,1,0,0,0,1};auto edgeTexture=FixtureTexture(2,edge);
    opaque.emission=edgeTexture.Get(); // mapST[8..11] is emission UV transform
    opaque.mapST[10]=.6f;opaque.sampling[3]=16;
    if(!render())return false;float repeat=scalar(view.color,center);
    opaque.sampling[3]=16|1|4;if(!render())return false;float clamp=scalar(view.color,center);
    result("texture_addressing",repeat==1 && clamp==0,repeat-clamp);
    // The authored specular map still modulates the skin's highlight strength.
    opaque=rtc::PathMaterial{};opaque.flags=8;opaque.surface=edgeTexture.Get();opaque.sampling[2]=16;
    opaque.skin[0]=.028f;for(int i=1;i<=5;i++)opaque.skin[i]=1;
    opaque.skin[18]=1.5f;opaque.skin[19]=1;
    if(!render())return false;
    auto spec=ReadTexture(view.specular,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    float lit=Half(spec[(64*128+32)*4]),dark=Half(spec[(64*128+96)*4]);
    result("skin_spatial_specular",lit>.01f && dark==0,lit-dark);
    // Independent source reference: the installed Legacy/Specular DXBC uses
    // MainTex.a * _SpecColor * (N.H)^(128*_Shininess), with unit incident light.
    // At frontal incidence this is .5*.4=.2, not a generic physical-F0 peak.
    rtc::Light frontal;frontal.id=700;frontal.type=rtc::LightType::Directional;
    frontal.direction[2]=1;frontal.angularRadius=0;for(auto& v:frontal.color)v=1;
    frame.lights=&frontal;frame.lightCount=1;
    opaque=rtc::PathMaterial{};opaque.flags=268435456;opaque.glossParams[1]=.25f;
    for(auto& v:opaque.baseColor)v=0;for(auto& v:opaque.specular)v=.4f;
    const float sourceGloss[]={1,1,1,.5f};auto sourceTexture=FixtureTexture(1,sourceGloss);opaque.base=sourceTexture.Get();
    if(!render())return false;float legacyPeak=scalar(view.color,center);
    result("legacy_authored_specular_peak",std::abs(legacyPeak-.2f)<.002f,legacyPeak);
    // Independent reference: the captured Skyshop Simple Glass DXBC has
    // SpecColor=.0481717847, SpecInt=1, Fresnel=0, SpecTex.a=1, Shininess=8.
    // Its frontal direct result is SpecColor*(256+2)/(4*pi)=.9890057.
    // This must survive the thin-sheet conversion; it is not water at .05.
    geo[0].normal={};geo[0].tangent={}; // normals must follow the reversed winding below
    // (0.22.68: on a generic sheet; an eye's wet film, below, is smooth instead.)
    opaque=rtc::PathMaterial{};opaque.flags=2048|512|67108864;
    opaque.transmission=1;opaque.ior=1.336f;opaque.roughness=.05f;
    opaque.glossParams[1]=8;opaque.glossParams[3]=0;
    for(auto& v:opaque.baseColor)v=0;
    opaque.specular[0]=.0481717847f;opaque.specular[1]=.02408589235f;opaque.specular[2]=0;
    if(!render())return false;
    float glassPeak=scalar(view.color,center),glassGreen=scalar(view.color,center+1),glassBlue=scalar(view.color,center+2);
    result("skyshop_sheet_source_peak",std::abs(glassPeak-.9890057f)<.03f,glassPeak);
    result("skyshop_sheet_reflection_tint",std::abs(glassGreen/glassPeak-.5f)<.003f && glassBlue<.001f,glassGreen/glassPeak);
    geo[0].indexBuffer=back.Get();if(!render())return false;float sheetBack=scalar(view.color,center);
    result("sheet_two_sided_interface",std::abs(glassPeak-sheetBack)<.001f,sheetBack);
    // 0.22.68 (test bit 22): the cornea / tear film (262144) is a wet interface at roughness .03, not the authored
    // gloss: the same sheet's highlight is sharper, its peak higher.
    geo[0].indexBuffer=ib.Get();opaque.flags|=262144;
    if(!render())return false;float wetPeak=scalar(view.color,center);
    result("wet_film_smooth",wetPeak>glassPeak*2,wetPeak);
    // 0.22.69 (bit 24): smooth, it reflects as tear on a cornea, ((1.336-1)/(1.336+1))^2 = .02066, the authored tint
    // kept (green half of red). The authored strength (bit 24 cleared): .0481717847 x 2/pi x 258/514 = .015393.
    float wetGreen=scalar(view.color,center+1);
    rtc::SetTestLayers(0xffffffffu&~(1u<<24));if(!render())return false;float wetAuthored=scalar(view.color,center);rtc::SetTestLayers(0xffffffffu);
    result("wet_film_physical_f0",std::abs(wetPeak/wetAuthored-1.3422f)<.03f && std::abs(wetGreen/wetPeak-.5f)<.01f,wetPeak/std::max(wetAuthored,1e-6f));
    opaque.materialControls[1]=0;
    if(!render())return false;float disabledReflection=scalar(view.color,center);
    result("sheet_zero_specular",disabledReflection==0,disabledReflection);
    // A disabled reflection sheet is clear, even when its source Color is
    // black/alpha zero. Its guide belongs to the emissive receiver behind it.
    layer=opaque;opaque=rtc::PathMaterial{};opaque.flags=1024;
    for(auto& v:opaque.emissionColor)v=1;
    frame.geometryCount=2;frame.lightCount=0;
    if(!render())return false;float clear=scalar(view.color,center);
    result("sheet_zero_clear_transport",std::abs(clear-1)<.002f && scalar(view.diffuse,center+3)==1,clear);
    // API 16 source-program contracts (pt-material-audit-20261102).
    auto region=[&](ID3D12Resource *texture,int channel) {
        auto pixels=ReadTexture(texture,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);double sum=0;
        for(int y=56;y<72;y++)for(int x=56;x<72;x++)sum+=Half(pixels[(y*128+x)*4+channel]);
        return float(sum/256);
    };
    frame.geometryCount=1;frame.lights=&frontal;frame.lightCount=1;view.samples=8;
    // VaM's x2(1-S/2) multiplies direct diffuse only; ambient sees the albedo.
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.3f;opaque.specular[c]=0;}opaque.roughness=1;
    opaque.materialControls[1]=0; // diffuse only
    if(!render())return false;float lightOne=region(view.color,0);
    for(int c=0;c<3;c++)opaque.directGain[c]=2;
    if(!render())return false;float lightTwo=region(view.color,0);
    frame.lightCount=0;frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;
    if(!render())return false;float ambientTwo=region(view.color,0);
    for(int c=0;c<3;c++)opaque.directGain[c]=1;
    if(!render())return false;float ambientOne=region(view.color,0);
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=0;frame.lightCount=1;
    result("direct_gain_light",std::abs(lightTwo/lightOne-2)<.01f,lightTwo/lightOne);
    result("direct_gain_not_ambient",std::abs(ambientTwo/ambientOne-1)<.02f,ambientTwo/ambientOne);
    // Decal over the tinted albedo by its own alpha: lerp(.2, (1,0,0), .5).
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.2f;opaque.specular[c]=0;}opaque.roughness=1;
    const float decalTexel[]={1,0,0,.5f};auto decalTexture=FixtureTexture(1,decalTexel);
    opaque.second=decalTexture.Get();opaque.mode=1;
    if(!render())return false;float decalRed=region(view.diffuse,0),decalGreen=region(view.diffuse,1);
    result("decal_composite",std::abs(decalRed-.6f)<.003f && std::abs(decalGreen-.1f)<.003f,decalRed);
    // Additive authoring: a strong authored specular no longer removes the
    // albedo (dielectric partition only), and yields to it instead.
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=.9f;}opaque.roughness=.3f;
    if(!render())return false;float partitioned=region(view.diffuse,0);
    opaque.mode=16;if(!render())return false;float additiveAlbedo=region(view.diffuse,0);
    result("authored_partition",partitioned<.25f,partitioned);
    result("additive_albedo_kept",std::abs(additiveAlbedo-.48f)<.003f,additiveAlbedo);
    // SeparateAlpha premultiplied programs: coverage .5 x closure x .5.
    frame.geometryCount=2;opaque=rtc::PathMaterial{};for(auto &v:opaque.baseColor)v=0;for(auto &v:opaque.specular)v=0;
    layer=rtc::PathMaterial{};for(int c=0;c<3;c++){layer.baseColor[c]=.6f;layer.specular[c]=0;}layer.roughness=1;
    const float halfAlpha[]={1,1,1,.5f};auto halfTexture=FixtureTexture(1,halfAlpha);layer.alpha=halfTexture.Get();
    if(!render())return false;float coveredPlain=region(view.color,0);
    layer.mode=4;if(!render())return false;float coveredSquared=region(view.color,0);
    result("coverage_closure",std::abs(coveredPlain-.3f)<.02f && std::abs(coveredSquared-.15f)<.012f,coveredSquared);
    // Skyshop's diffuse (_ExposureIBL.x) and reflection (.y) scales are separate.
    frame.geometryCount=1;frame.lightCount=0;
    auto cube=Cube();rtc::PathEnvironment sky;sky.lighting=cube.Get();sky.rgbm=0;sky.lightingScale=1;sky.diffuseScale=0;
    if(!cube || !rtc::SetPathEnvironment(sky))return false;
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;
    opaque.materialControls[1]=0; // no grazing Schlick term: diffuse only
    if(!render())return false;float diffuseOff=region(view.color,0);
    sky.diffuseScale=1;rtc::SetPathEnvironment(sky);
    if(!render())return false;float diffuseOn=region(view.color,0);
    for(int c=0;c<3;c++){opaque.baseColor[c]=0;opaque.specular[c]=.9f;}opaque.roughness=.05f;opaque.materialControls[1]=1;sky.diffuseScale=0;rtc::SetPathEnvironment(sky);
    view.maxBounces=2; // a sharp reflection is the BSDF continuation's miss
    if(!render())return false;float reflection=region(view.color,0);
    view.maxBounces=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    result("environment_diffuse_off",diffuseOff<1e-4f,diffuseOff);
    result("environment_diffuse_on",diffuseOn>.1f,diffuseOn);
    result("environment_reflection",reflection>.5f,reflection);
    // API 17 light-path contracts (pt-light-audit-20261102).
    // A hidden sky shows the camera's clear colour, not the lighting cube.
    frame.geometryCount=0;
    sky=rtc::PathEnvironment{};sky.lighting=cube.Get();sky.rgbm=0;sky.lightingScale=1;sky.backgroundScale=0;
    sky.backgroundColor[0]=.2f;sky.backgroundColor[1]=.4f;sky.backgroundColor[2]=.6f;rtc::SetPathEnvironment(sky);
    if(!render())return false;float clearRed=scalar(view.color,center),clearBlue=scalar(view.color,center+2);
    result("background_clear_colour",std::abs(clearRed-.2f)<.002f && std::abs(clearBlue-.6f)<.003f,clearBlue);
    // A ShadowsOnly proxy in front of the receiver: the camera sees the
    // receiver, the analytic light is blocked, the environment is not.
    rtc::Submesh proxySub=sub[0];geo[1].submeshes=&proxySub;geo[1].matrix[11]=-.1f;
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
    frame.geometryCount=2;frame.lights=&frontal;frame.lightCount=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    if(!render())return false;float proxySeen=region(view.color,0);
    geo[1].flags=rtc::kRigid|rtc::kShadowOnly;
    if(!render())return false;float proxyShadow=region(view.color,0);
    result("shadow_only_hidden_and_blocking",proxySeen>.2f && proxyShadow<1e-4f,proxyShadow);
    frame.lightCount=0;sky=rtc::PathEnvironment{};sky.lighting=cube.Get();sky.rgbm=0;sky.diffuseScale=1;rtc::SetPathEnvironment(sky);
    if(!render())return false;float proxyEnv=region(view.color,0);
    frame.geometryCount=1;if(!render())return false;float openEnv=region(view.color,0);
    result("shadow_only_environment_open",std::abs(proxyEnv/openEnv-1)<.03f,proxyEnv/openEnv);
    // The range window: the author's 1/(1+25x^2) inside 0.8 of the range,
    // smoothstep(1,.64,x^2) = .5416 at x = .9, nothing past the range.
    rtc::SetPathEnvironment(rtc::PathEnvironment{});geo[1].flags=rtc::kRigid;
    rtc::Light point;point.id=701;point.type=rtc::LightType::Point;point.position[2]=0;point.radius=0;point.angularRadius=0;
    for(auto &v:point.color)v=1;frame.lights=&point;frame.lightCount=1;
    float falloff[3];const float ranges[]={1/.5f,1/.9f,1/1.01f}; // receiver 1 m away: x = .5, .9, 1.01
    for(int i=0;i<3;i++){point.range=ranges[i];if(!render())return false;falloff[i]=scalar(view.color,center);}
    float inner=falloff[0]*(1+25*.25f),window=falloff[1]*(1+25*.81f)/inner;
    result("range_window",inner>.1f && std::abs(window-.5416f)<.01f && falloff[2]==0,window);
    // Light linking (API 18, pt-light-link-20261102; Unity's cullingMask): a
    // light that leaves an object's class out neither lights it nor is
    // shadowed by it; other classes and the environment are unaffected.
    {
        frame.lights=&frontal;frame.lightCount=1;frame.geometryCount=1;view.samples=8;
        auto linkedLight=[&](uint32_t receiver,uint32_t excluded) {
            geo[0].lightLink=receiver;frontal.excludedLinks=excluded;
            return render()?region(view.color,0):-1.f;
        };
        float open=linkedLight(0,0),own=linkedLight(1,1),other=linkedLight(1,2),unlinked=linkedLight(0,1);
        result("link_unlit_receiver",open>.1f && own==0 && std::abs(other/open-1)<.01f && std::abs(unlinked/open-1)<.01f,own);
        geo[0].lightLink=0;geo[1].submeshes=&proxySub;geo[1].matrix[11]=-.1f;geo[1].flags=rtc::kRigid|rtc::kShadowOnly;frame.geometryCount=2;
        geo[1].lightLink=1;frontal.excludedLinks=0;if(!render())return false;float blocked=region(view.color,0);
        frontal.excludedLinks=1;if(!render())return false;float passed=region(view.color,0);
        frontal.excludedLinks=2;if(!render())return false;float otherBlocked=region(view.color,0);
        result("link_no_shadow",blocked<1e-4f && std::abs(passed/open-1)<.01f && otherBlocked<1e-4f,passed/std::max(open,1e-6f));
        geo[1].lightLink=0;geo[1].flags=rtc::kRigid;geo[1].submeshes=&sub[1];geo[1].matrix[11]=-.1f;frontal.excludedLinks=0;
        frame.geometryCount=1;frame.lightCount=0;view.samples=1;
    }
    // RR guide contracts (pt-rr-audit-20261102).
    // A sky pixel's specular albedo is .5 (DLSS-RR guide 3.4.2), never zero.
    frame.lightCount=0;frame.geometryCount=0;view.samples=1;
    if(!render())return false;
    float skySpec=scalar(view.specular,center),skyDiffuse=scalar(view.diffuse,center);
    result("sky_specular_guide",skySpec==.5f && skyDiffuse==0,skySpec);
    // An attenuation layer scales both albedo guides by its transmission (.75);
    // what it absorbs is not specular reflectance.
    opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=.3f;}opaque.roughness=.5f;
    layer=rtc::PathMaterial{};layer.flags=2048|1048576;layer.baseColor[3]=.25f;layer.transmission=1;
    geo[1].submeshes=&sub[1];frame.geometryCount=1;
    if(!render())return false;float bareSpec=scalar(view.specular,center),bareDiffuse=scalar(view.diffuse,center);
    frame.geometryCount=2;
    if(!render())return false;float tintSpec=scalar(view.specular,center),tintDiffuse=scalar(view.diffuse,center);
    result("attenuation_specular_guide",bareSpec>.01f && std::abs(tintSpec-.75f*bareSpec)<.003f && std::abs(tintDiffuse-.75f*bareDiffuse)<.003f,tintSpec/std::max(bareSpec,1e-6f));
    // A sampled reflection that escapes reports the environment as far away;
    // zero stays reserved for "specular lobe not sampled".
    frame.geometryCount=1;view.maxBounces=2;
    for(int c=0;c<3;c++){opaque.baseColor[c]=0;opaque.specular[c]=.9f;}opaque.roughness=.05f;opaque.materialControls[1]=1;
    if(!render())return false;
    {
        auto hits=ReadTexture(view.hitDistance,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1);unsigned distant=0,between=0;
        for(int y=56;y<72;y++)for(int x=56;x<72;x++){float d=Half(hits[y*128+x]);distant+=d>9000;between+=d>0 && d<=9000;}
        result("missed_reflection_distance",distant>=200 && between==0,distant/256.f);
    }
    // RR F response hint: verify implementation boundaries, not neural-network
    // image quality. Only a new R16F output is allowed to differ.
    {
        const auto savedView=view;const auto savedOpaque=opaque,savedLayer=layer;
        const auto savedGeometryCount=frame.geometryCount,savedLightCount=frame.lightCount;
        const auto savedLights=frame.lights;
        auto mask=Tex(128,128,DXGI_FORMAT_R16_FLOAT);
        auto motion=Tex(128,128,DXGI_FORMAT_R32G32_FLOAT);
        rtc::PathResponsivity response;response.mask=mask.Get();response.strength=.25f;
        response.motionScale[0]=response.motionScale[1]=60;
        view.motion=motion.Get();view.samples=1;view.maxBounces=2;view.seed=7;
        view.jitter[0]=view.jitter[1]=0;memcpy(view.previousWorldToClip,view.worldToClip,64);
        opaque=rtc::PathMaterial{};opaque.flags=8;opaque.roughness=.5f;
        for(int c=0;c<3;c++)opaque.baseColor[c]=.5f;
        layer=rtc::PathMaterial{};frame.geometryCount=1;frame.lightCount=1;frame.lights=&frontal;
        auto snapshot=[&]() {
            std::vector<std::vector<uint16_t>> values;
            for(auto *t:{view.color,view.diffuse,view.normal,view.specular})
                values.push_back(ReadTexture(t,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4));
            for(auto *t:{view.depth,view.hitDistance})
                values.push_back(ReadTexture(t,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1));
            return values;
        };
        auto maskValues=[&](){return ReadTexture(mask.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1);};
        auto centerMask=[&](){return Half(maskValues()[center/4]);};
        responseTest=nullptr;if(!render())return false;auto without=snapshot();
        responseTest=&response;if(!render())return false;auto with=snapshot();float still=centerMask();
        result("response_existing_outputs_unchanged",without==with,without==with?1.f:0.f);
        result("response_static_skin",still==.25f,still);
        auto initialMask=maskValues();view.seed=43;if(!render())return false;
        result("response_seed_independent",initialMask==maskValues(),centerMask());
        view.jitter[0]=.3f;view.jitter[1]=-.2f;if(!render())return false;
        result("response_jitter_not_motion",centerMask()==.25f,centerMask());
        view.jitter[0]=view.jitter[1]=0;
        auto speedMask=[&](float pixelsPerFrame,float scale) {
            memcpy(view.previousWorldToClip,view.worldToClip,64);
            const float ndc=2*pixelsPerFrame/128;
            for(int k=0;k<4;k++)view.previousWorldToClip[k]+=ndc*view.worldToClip[12+k];
            response.motionScale[0]=response.motionScale[1]=scale;
            return render()?centerMask():-1.f;
        };
        const float middle=speedMask(7.f/60,60),fast=speedMask(.25f,60),sameSpeed=speedMask(7.f/120,120);
        result("response_motion_fades",std::abs(middle-.125f)<.002f && fast==0,middle);
        result("response_frame_rate_independent",std::abs(middle-sameSpeed)<.002f,sameSpeed);
        memcpy(view.previousWorldToClip,view.worldToClip,64);
        response.motionScale[0]=response.motionScale[1]=60;
        auto noHint=[&](){if(!render())return false;auto pixels=maskValues();return std::all_of(pixels.begin(),pixels.end(),[](uint16_t v){return v==0;});};
        opaque.flags=0;result("response_non_skin_zero",noHint(),0);
        opaque.flags=8;opaque.transmission=.5f;result("response_transmission_zero",noHint(),0);
        opaque.transmission=0;frame.geometryCount=2;layer.alpha=alpha.Get();
        result("response_coverage_zero",noHint(),0);
        layer=rtc::PathMaterial{};layer.flags=2048;layer.transmission=1;layer.roughness=.03f;
        result("response_clear_interface_zero",noHint(),0);
        frame.geometryCount=0;result("response_sky_clears_previous",noHint(),0);
        frame.geometryCount=1;response.strength=0;result("response_zero_strength",noHint(),0);
        response.strength=1;if(!render())return false;
        auto full=maskValues();bool range=true;for(auto x:full){float f=Half(x);range&=std::isfinite(f) && f>=0 && f<=1;}
        result("response_nominal_range",range && centerMask()==1,centerMask());
        responseTest=nullptr;view=savedView;opaque=savedOpaque;layer=savedLayer;
        frame.geometryCount=savedGeometryCount;frame.lightCount=savedLightCount;frame.lights=savedLights;
    }
    view.maxBounces=1;
    // Body audit contracts (pt-body-audit-20261102).
    // A dense hair volume (24 layers, volume fraction about .19) under uniform
    // light: scattering among its fibres is the volume's own transport and
    // must not depend on the scene bounce budget (4 against 8).
    {
        constexpr unsigned rows=48,layers=24,segments=16,strands=rows*layers,points=strands*(segments+1);
        std::vector<uint32_t> fibres(((points+31)/32)*32*9,0);
        auto half=[](float v){uint32_t b;memcpy(&b,&v,4);int e=int((b>>23)&255)-127+15;return uint16_t(v<=0?0:((b>>16)&0x8000)|(e<<10)|((b>>13)&1023));};
        auto hairColour=[&](float c) {
            for(unsigned s=0;s<strands;s++) {
                unsigned row=s%rows,layer=s/rows;uint32_t h=(s*2654435761u)^0x9e3779b9u;h^=h>>15;h*=2246822519u;h^=h>>13;
                float y=-1+(row+(h&1023)/1023.f)*(2.f/rows),z=-1+(layer+.5f)*(.6f/layers);
                for(unsigned k=0;k<=segments;k++) {
                    unsigned v=s*(segments+1)+k;uint32_t *block=fibres.data()+(v/32)*32*9;
                    float p[3]={-2+4.f*k/segments,y,z};memcpy(block+(v%32)*3,p,12);
                    block[192+(v%32)*3+1]=half(c)|uint32_t(half(c))<<16;block[192+(v%32)*3+2]=half(c);
                }
            }
            return Data(fibres.data(),fibres.size()*4);
        };
        PathHairItem item;item.strands=strands;item.segments=segments;item.radius=.008f;item.roughness=.15f;
        frame.geometryCount=0;frame.lightCount=0;view.samples=8;
        sky=rtc::PathEnvironment{};sky.lighting=cube.Get();sky.rgbm=0;sky.lightingScale=sky.diffuseScale=1;rtc::SetPathEnvironment(sky);
        float slab[2][2]={};bool finite=true;
        for(int colour=0;colour<2;colour++) {
            auto fibreBuffer=hairColour(colour?.12f:.9f);
            for(int budget=0;budget<2;budget++) {
                view.maxBounces=budget?8:4;double sum=0;
                for(unsigned seed=0;seed<6;seed++) {
                    view.seed=900+seed;
                    Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathHair(G.list.Get(),fibreBuffer.Get(),&item,1,frame.frame)||!rtc::RecordPathTrace(G.list.Get(),view))return false;
                    SubmitWait();Drain("hair volume");frame.completedFrame=frame.frame++;
                    auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
                    for(int y=48;y<80;y++)for(int x=48;x<80;x++){float g=Half(pixels[(y*128+x)*4+1]);finite&=std::isfinite(g);sum+=g;}
                }
                slab[colour][budget]=float(sum/(6*32*32));
            }
        }
        Begin();rtc::RecordPathHair(G.list.Get(),nullptr,nullptr,0,0);SubmitWait();Drain("hair volume release");
        rtc::SetPathEnvironment(rtc::PathEnvironment{});view.maxBounces=1;view.samples=1;
        char text[256];snprintf(text,sizeof text,",\"hair_volume_levels\":{\"light4\":%.6g,\"light8\":%.6g,\"dark4\":%.6g,\"dark8\":%.6g}",slab[0][0],slab[0][1],slab[1][0],slab[1][1]);json+=text;
        result("hair_volume_budget_light",finite && slab[0][0]>.05f && std::abs(slab[0][0]/slab[0][1]-1)<.03f,slab[0][0]/std::max(slab[0][1],1e-6f));
        result("hair_volume_budget_dark",finite && slab[1][0]>.002f && std::abs(slab[1][0]/slab[1][1]-1)<.03f,slab[1][0]/std::max(slab[1][1],1e-6f));
        // Two light-link classes are two fiber BLASes: the same strands split
        // into an unlinked and a linked item render as one item (environment
        // light is not linked; a fiber hit finds its item across instances).
        {
            auto fibreBuffer=hairColour(.9f);view.maxBounces=4;view.samples=8;rtc::SetPathEnvironment(sky);
            PathHairItem split[2]={item,item};split[0].strands=strands/2;split[1].strands=strands-strands/2;
            split[1].firstPoint=split[0].strands*(segments+1);split[1].pad=1;
            double sum[2]={};
            for(int variant=0;variant<2;variant++)for(unsigned seed=0;seed<6;seed++) {
                view.seed=900+seed;
                Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathHair(G.list.Get(),fibreBuffer.Get(),variant?split:&item,variant?2:1,frame.frame)||!rtc::RecordPathTrace(G.list.Get(),view))return false;
                SubmitWait();Drain("hair link");frame.completedFrame=frame.frame++;
                auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
                for(int y=48;y<80;y++)for(int x=48;x<80;x++)sum[variant]+=Half(pixels[(y*128+x)*4+1]);
            }
            Begin();rtc::RecordPathHair(G.list.Get(),nullptr,nullptr,0,0);SubmitWait();Drain("hair link release");
            result("link_hair_split",sum[0]>0 && std::abs(sum[1]/sum[0]-1)<.03f,float(sum[1]/std::max(sum[0],1e-9)));
            rtc::SetPathEnvironment(rtc::PathEnvironment{});view.maxBounces=1;view.samples=1;
        }
        // Camera-hidden fibres (API 21, pt-embody-head-20261103): a first-person
        // plugin's hidden hair between the camera and a sky-lit receiver. The
        // camera sees the receiver through it; its sky light stays shaded by it.
        {
            auto fibreBuffer=hairColour(.9f);view.maxBounces=1;view.samples=8;rtc::SetPathEnvironment(sky);
            opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
            if(!rtc::SetPathMaterial(sub[0].material,opaque))return false;
            geo[0].flags=rtc::kRigid;geo[0].lightLink=0;frame.geometryCount=1;frame.lightCount=0;
            PathHairItem hidden=item;hidden.pad=kPathHairCameraHidden;
            double plane[3]={};float hiddenGuides[3]={};
            for(int variant=0;variant<3;variant++) { // no hair, hidden hair, seen hair
                for(unsigned seed=0;seed<6;seed++) {
                    view.seed=950+seed;
                    Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathHair(G.list.Get(),variant?fibreBuffer.Get():nullptr,variant==1?&hidden:&item,variant?1:0,frame.frame)||!rtc::RecordPathTrace(G.list.Get(),view))return false;
                    SubmitWait();Drain("hair camera");frame.completedFrame=frame.frame++;
                    plane[variant]+=region(view.color,1)/6;
                }
                hiddenGuides[variant]=scalar(view.diffuse,center+3);
            }
            Begin();rtc::RecordPathHair(G.list.Get(),nullptr,nullptr,0,0);SubmitWait();Drain("hair camera release");
            result("camera_hidden_hair_seen_through",hiddenGuides[0]==1 && hiddenGuides[1]==1 && hiddenGuides[2]<0,hiddenGuides[1]);
            result("camera_hidden_hair_shades",plane[0]>.01 && plane[1]<.97*plane[0],float(plane[1]/std::max(plane[0],1e-9)));
            rtc::SetPathEnvironment(rtc::PathEnvironment{});view.maxBounces=1;view.samples=1;
        }
    }
    // Camera-hidden geometry (API 21, pt-embody-head-20261103): a first-person
    // plugin's head in front of the receiver. The camera sees the receiver;
    // the analytic light and the environment are blocked as by any surface.
    {
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        rtc::Submesh headSub=sub[0];geo[1].submeshes=&headSub;geo[1].matrix[11]=-.1f;geo[1].flags=rtc::kRigid;geo[1].lightLink=0;
        geo[0].flags=rtc::kRigid;geo[0].lightLink=0;frontal.excludedLinks=0;
        frame.geometryCount=2;frame.lights=&frontal;frame.lightCount=1;view.samples=1;view.maxBounces=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;float seenGuide=scalar(view.diffuse,center+3);
        geo[1].flags=rtc::kRigid|rtc::kCameraHidden;
        if(!render())return false;float hiddenGuide=scalar(view.diffuse,center+3),hiddenLight=region(view.color,0);
        frame.lightCount=0;rtc::SetPathEnvironment(sky);view.samples=8;
        if(!render())return false;float hiddenEnv=region(view.color,0);
        frame.geometryCount=1;if(!render())return false;float bareEnv=region(view.color,0);
        rtc::SetPathEnvironment(rtc::PathEnvironment{});view.samples=1;geo[1].flags=rtc::kRigid;
        result("camera_hidden_seen_through",seenGuide==2 && hiddenGuide==1,hiddenGuide);
        result("camera_hidden_blocks_light",hiddenLight<1e-4f,hiddenLight);
        result("camera_hidden_blocks_environment",bareEnv>.05f && hiddenEnv<.2f*bareEnv,hiddenEnv/std::max(bareEnv,1e-6f));
    }
    // Grazing shading normals (pt-silhouette-20261103): on a smooth body's silhouette the
    // interpolated normal leans away from the eye while the triangle faces it. It shades with
    // the triangle's normal, not black; diffuse keeps its own normal. Centre pixel, light above.
    {
        const float leaning[]={0,1,.05f,0,1,.05f,0,1,.05f,0,1,.05f};auto lb=Data(leaning,sizeof leaning);
        rtc::Light above;above.id=702;above.type=rtc::LightType::Directional;
        above.direction[1]=-.6f;above.direction[2]=.8f;above.angularRadius=0;for(auto &v:above.color)v=1;
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        geo[0].flags=rtc::kRigid;frame.geometryCount=1;frame.lights=&above;frame.lightCount=1;view.samples=1;view.maxBounces=1;
        geo[0].normal={nb.Get(),0,12,rtc::VertexFormat::Float3};
        if(!render())return false;float facingLit=scalar(view.color,center);
        geo[0].normal={lb.Get(),0,12,rtc::VertexFormat::Float3};
        if(!render())return false;float leaningLit=scalar(view.color,center);
        geo[0].normal={};frame.lightCount=0;
        // diffuse N.L: dot(normalize(0,1,.05),l) / dot(-z,l) = .5593 / .8
        result("grazing_normal_shaded",facingLit>.05f && std::abs(leaningLit/facingLit-.6991f)<.01f,leaningLit/std::max(facingLit,1e-6f));
    }
    // Eye audit contracts (pt-eye-audit-20261103; RtCore test bits 20, 21 cleared = 0.22.66).
    // A clear sheet in front of a matte, sky-lit receiver (an eye's film over the eyeball). With no diffuse bounce
    // left the receiver's sky is its environment sample alone: 0.22.66 stopped it at the sheet (bit 21 cleared: only
    // the sheet's own reflection is left); now it passes as analytic light does, the camera ray and the sample each
    // losing the sheet's Fresnel (about .98 x .98 at IOR 1.336). With bounces the continuation's sky is weighed
    // against those samples: the same mean as 0.22.66's continuation alone.
    {
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        layer=rtc::PathMaterial{};layer.flags=2048;layer.transmission=1;layer.ior=1.336f;layer.roughness=.03f;
        geo[0].flags=rtc::kRigid;geo[0].lightLink=0;geo[1].flags=rtc::kRigid;geo[1].lightLink=0;geo[1].submeshes=&sub[1];geo[1].matrix[11]=-.1f;
        frame.lightCount=0;view.maxBounces=2;view.samples=8;
        sky=rtc::PathEnvironment{};sky.lighting=cube.Get();sky.rgbm=0;sky.lightingScale=sky.diffuseScale=1;
        auto envMean=[&](unsigned geometries,uint32_t bounces,uint32_t layers) {
            frame.geometryCount=geometries;sky.diffuseBounces=bounces;rtc::SetPathEnvironment(sky);rtc::SetTestLayers(layers);
            double sum=0;
            for(unsigned seed=0;seed<8;seed++){view.seed=1000+seed;if(!render())return -1.f;sum+=region(view.color,1);}
            return float(sum/8);
        };
        const uint32_t all=0xffffffffu,old=all&~(1u<<21);
        float bare0=envMean(1,0,all),film0=envMean(2,0,all),old0=envMean(2,0,old);
        float bare8=envMean(1,8,all),film8=envMean(2,8,all),old8=envMean(2,8,old);
        rtc::SetTestLayers(all);rtc::SetPathEnvironment(rtc::PathEnvironment{});view.maxBounces=1;view.samples=1;frame.geometryCount=1;
        char levelText[256];snprintf(levelText,sizeof levelText,",\"film_environment_levels\":{\"bare0\":%.6g,\"film0\":%.6g,\"old0\":%.6g,\"bare8\":%.6g,\"film8\":%.6g,\"old8\":%.6g}",bare0,film0,old0,bare8,film8,old8);json+=levelText;
        float passed=(film0-old0)/std::max(bare0,1e-6f);
        result("film_environment_sample",bare0>.05f && old0<.1f*bare0 && passed>.9f && passed<.99f,passed);
        result("film_environment_weighed",old8>.05f && std::abs(film8/old8-1)<.03f,film8/std::max(old8,1e-6f));
    }
    // A metal mirror under a point light of 8 degrees in front of it: the light's image. A point drawn over the disc
    // hardly ever lands in a .03 lobe (bit 20 cleared, 0.22.66); a draw from the lobe that lands on the disc meets it.
    // A glossy (.2) one, narrower than the disc too, keeps its mean with either; the mirror's matches it.
    {
        rtc::Light lamp;lamp.id=703;lamp.type=rtc::LightType::Point;lamp.position[2]=-1;lamp.radius=0;lamp.angularRadius=.1396f;lamp.range=100;
        for(auto &v:lamp.color)v=1;
        frame.lights=&lamp;frame.lightCount=1;frame.geometryCount=1;geo[0].flags=rtc::kRigid;view.maxBounces=1;view.samples=8;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        auto spot=[&](float roughness,uint32_t layers,double &noise) {
            opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=0;opaque.specular[c]=.9f;}opaque.roughness=roughness;opaque.materialControls[1]=1;
            rtc::SetTestLayers(layers);
            std::vector<double> sum(48*48,0),square(48*48,0);
            for(unsigned seed=0;seed<16;seed++) {
                view.seed=1100+seed;if(!render())return -1.0;
                auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
                for(int y=0;y<48;y++)for(int x=0;x<48;x++){double g=Half(pixels[((y+40)*128+x+40)*4+1]);sum[y*48+x]+=g/16;square[y*48+x]+=g*g/16;}
            }
            double total=0,variance=0;for(size_t i=0;i<sum.size();i++){total+=sum[i];variance+=std::max(square[i]-sum[i]*sum[i],0.0);}
            noise=std::sqrt(variance/16)/std::max(total,1e-9); // the sum's relative standard error over the 16 seeds
            return total;
        };
        const uint32_t all=0xffffffffu,old=all&~(1u<<20);double noise[4]{};
        double glossyNew=spot(.2f,all,noise[0]),glossyOld=spot(.2f,old,noise[1]),sharpNew=spot(.03f,all,noise[2]),sharpOld=spot(.03f,old,noise[3]);
        rtc::SetTestLayers(all);frame.lights=&frontal;frame.lightCount=0;view.samples=1;
        char levelText[320];snprintf(levelText,sizeof levelText,",\"lobe_light_levels\":{\"glossy\":[%.6g,%.6g],\"sharp\":[%.6g,%.6g],\"noise\":[%.4g,%.4g,%.4g,%.4g]}",glossyNew,glossyOld,sharpNew,sharpOld,noise[0],noise[1],noise[2],noise[3]);json+=levelText;
        result("lobe_light_glossy_unchanged",glossyOld>0 && std::abs(glossyNew/glossyOld-1)<.03,float(glossyNew/std::max(glossyOld,1e-9)));
        result("lobe_light_sharp_found",sharpNew>.85*glossyNew && sharpNew<1.15*glossyNew && noise[2]<.05,float(sharpNew/std::max(glossyNew,1e-9)));
    }
    // VaM's Subsurface family lit directly (0.22.69, bit 23; cleared: (2-S) N.L, 0.22.68): a matte family plane under a
    // sun head-on and at N.L .5. 2(1-S/2) saturate(S/2+(1-S/2)N.L) ((N.L+1)/2)^2: S 0 .28125 of head-on, S .4 .3375.
    {
        rtc::Light sun;sun.id=704;sun.type=rtc::LightType::Directional;sun.angularRadius=0;for(auto &v:sun.color)v=1;
        frame.lights=&sun;frame.lightCount=1;frame.geometryCount=1;geo[0].flags=rtc::kRigid;view.maxBounces=1;view.samples=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        auto familyLit=[&](float S,float tilt,uint32_t layers) {
            opaque=rtc::PathMaterial{};opaque.flags=33554432;opaque.roughness=1;opaque.materialControls[1]=0;
            for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;opaque.directGain[c]=2-S;}
            sun.direction[0]=std::sin(tilt);sun.direction[1]=0;sun.direction[2]=std::cos(tilt);
            rtc::SetTestLayers(layers);return render()?scalar(view.color,center):-1.f;
        };
        const uint32_t all=0xffffffffu,old=all&~(1u<<23);const float sixty=1.04719755f;
        float s0=familyLit(0,sixty,all)/familyLit(0,0,all),s4=familyLit(.4f,sixty,all)/familyLit(.4f,0,all),o0=familyLit(0,sixty,old)/familyLit(0,0,old);
        rtc::SetTestLayers(all);frame.lightCount=0;
        char levelText[160];snprintf(levelText,sizeof levelText,",\"family_direct_levels\":{\"s0\":%.6g,\"s04\":%.6g,\"old\":%.6g}",s0,s4,o0);json+=levelText;
        result("family_direct_falloff",std::abs(s0/.28125f-1)<.015f && std::abs(s4/.3375f-1)<.015f && std::abs(o0/.5f-1)<.015f,s0);
    }
    // An eyeball under its lid (0.22.70, bit 25; cleared: 0.22.69): a matte plane 0.1 m from the camera, a sun head-on,
    // and a wall standing on it 4 mm beside the centre column (parallel to the sun: no shadow). Flagged an eyeball, the
    // column's sun light is the open share of its hemisphere within 12 mm: a wall at h = .27-.33 of the range cuts
    // (acos h - h sqrt(1-h^2)) / pi = .29-.33 of it, .67-.71 left. Without the wall, or unflagged, nothing changes.
    {
        rtc::Light sun;sun.id=705;sun.type=rtc::LightType::Directional;sun.angularRadius=0;for(auto &v:sun.color)v=1;
        sun.direction[0]=sun.direction[1]=0;sun.direction[2]=1;
        frame.lights=&sun;frame.lightCount=1;geo[0].flags=rtc::kRigid;geo[1].flags=rtc::kRigid;view.maxBounces=1;view.samples=8;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        const float keep0=geo[0].matrix[11];float keep1[12];memcpy(keep1,geo[1].matrix,sizeof keep1);const rtc::Submesh *keepSub=geo[1].submeshes;
        geo[0].matrix[11]=-3.9f;
        float wall[12]={0,0,1,-.996f, 0,1,0,0, -1,0,0,-2.9f};memcpy(geo[1].matrix,wall,sizeof wall);geo[1].submeshes=&sub[0];
        auto column=[&](uint32_t flags,unsigned geometries,uint32_t layers) {
            opaque=rtc::PathMaterial{};opaque.flags=flags;opaque.roughness=1;opaque.materialControls[1]=0;
            for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}
            frame.geometryCount=geometries;rtc::SetTestLayers(layers);
            double sum=0;
            for(unsigned seed=0;seed<4;seed++) {
                view.seed=1200+seed;if(!render())return -1.0;
                auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
                for(int y=32;y<96;y++)sum+=Half(pixels[(y*128+64)*4+1]);
            }
            return sum/(4*64);
        };
        const uint32_t all=0xffffffffu,old=all&~(1u<<25),eye=1073741824u;
        double lidded=column(eye,2,all),bare=column(eye,2,old),open=column(eye,1,all),openOld=column(eye,1,old);
        double plain=column(0,2,all),plainOld=column(0,2,old);
        geo[0].matrix[11]=keep0;memcpy(geo[1].matrix,keep1,sizeof keep1);geo[1].submeshes=keepSub;
        rtc::SetTestLayers(all);frame.lightCount=0;frame.geometryCount=1;view.samples=1;
        char levelText[256];snprintf(levelText,sizeof levelText,",\"eyelid_levels\":{\"lidded\":%.6g,\"bare\":%.6g,\"open\":%.6g,\"open_old\":%.6g,\"plain\":%.6g,\"plain_old\":%.6g}",
            lidded,bare,open,openOld,plain,plainOld);json+=levelText;
        result("eyelid_shuts_light",bare>.05 && lidded/bare>.6 && lidded/bare<.78,float(lidded/std::max(bare,1e-9)));
        result("eyelid_open_unchanged",openOld>.05 && std::abs(open/openOld-1)<1e-4 && plain==plainOld && std::abs(bare/openOld-1)<1e-3,float(open/std::max(openOld,1e-9)));
    }
    // The eyelid band on an eyeball (0.22.71, bit 26; cleared: 0.22.70): the matte plane 0.1 m from the camera under a
    // sun head-on, flagged an eyeball, with one eye rig - an eyeball of radius .2 m behind the plane, touching it at the
    // centre, and an upper lid's contact line on its sphere 5 mm beside the centre column, the band's width .025 R = 5 mm.
    // Each column pixel's light is scaled by 1 - .55 e^(-d^2/wu^2) at its point on the sphere (gen_spec_table.py
    // EyeOcclusion, here on the CPU at the pixel centres); with no rig, the bit cleared or the plane unflagged, nothing
    // changes.
    {
        rtc::Light sun;sun.id=706;sun.type=rtc::LightType::Directional;sun.angularRadius=0;for(auto &v:sun.color)v=1;
        sun.direction[0]=sun.direction[1]=0;sun.direction[2]=1;
        frame.lights=&sun;frame.lightCount=1;frame.geometryCount=1;geo[0].flags=rtc::kRigid;view.maxBounces=1;view.samples=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        const float keep0=geo[0].matrix[11];geo[0].matrix[11]=-3.9f;
        const double R=.2,cz=-2.9+R,lineX=.00578125;
        const uint32_t points=41;double line[points][4];
        std::vector<float> rig(rtc::kPathEyeRigBytes/4,0.f);
        memcpy(&rig[0],&points,4);
        const float occ[4]={.55f,.025f,.3f,.066f},centre[4]={0,0,(float)cz,1},gaze[4]={0,0,-1,(float)R};
        memcpy(&rig[8],occ,16);memcpy(&rig[12],centre,16);memcpy(&rig[16],gaze,16);
        for(uint32_t k=0;k<points;k++) {
            const double y=-.08+.004*k;
            line[k][0]=lineX;line[k][1]=y;line[k][2]=cz-std::sqrt(R*R-lineX*lineX-y*y);line[k][3]=k?1:3;
            for(int i=0;i<4;i++)rig[28+4*k+i]=(float)line[k][i];
        }
        auto rigBuffer=Data(rig.data(),rig.size()*4);
        auto column=[&](uint32_t flags,bool rigged,uint32_t layers) {
            opaque=rtc::PathMaterial{};opaque.flags=flags;opaque.roughness=1;opaque.materialControls[1]=0;
            for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}
            rtc::SetPathEyeRigs(rigged?rigBuffer.Get():nullptr,rigged?1:0);rtc::SetTestLayers(layers);
            if(!render())return -1.0;
            auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);double sum=0;
            for(int y=32;y<96;y++)sum+=Half(pixels[(y*128+64)*4+1]);
            return sum/64;
        };
        double expected=0;  // the band's mean over the column (the probe camera's 90 degrees, top-left origin)
        for(int y=32;y<96;y++) {
            const double px=.1*((64.5/128)*2-1),py=.1*(1-2*((y+.5)/128)),dz=-2.9-cz;
            const double len=std::sqrt(px*px+py*py+dz*dz),q[3]={px/len*R,py/len*R,cz+dz/len*R},wu=.025*R;
            double du=0;
            for(uint32_t k=1;k<points;k++) {
                const double *a=line[k-1],*b=line[k];double ab[3],abab=0,t=0,d2=0;
                for(int i=0;i<3;i++){ab[i]=b[i]-a[i];abab+=ab[i]*ab[i];t+=(q[i]-a[i])*ab[i];}
                t=std::clamp(t/abab,0.,1.);
                for(int i=0;i<3;i++){const double d=q[i]-(a[i]+t*ab[i]);d2+=d*d;}
                du=std::max(du,((a[3]>=2?a[3]-2:a[3])*(1-t)+b[3]*t)*std::exp(-d2/(wu*wu)));
            }
            expected+=(1-.55*du)/64;
        }
        const uint32_t all=0xffffffffu,old=all&~(1u<<26),eye=1073741824u;
        double banded=column(eye,true,all),bare=column(eye,true,old),noRig=column(eye,false,all);
        double plain=column(0,true,all),plainOld=column(0,true,old);
        rtc::SetPathEyeRigs(nullptr,0);geo[0].matrix[11]=keep0;
        rtc::SetTestLayers(all);frame.lightCount=0;frame.geometryCount=1;view.samples=1;
        char levelText[256];snprintf(levelText,sizeof levelText,",\"eyelid_band_levels\":{\"banded\":%.6g,\"bare\":%.6g,\"no_rig\":%.6g,\"plain\":%.6g,\"plain_old\":%.6g,\"expected\":%.6g}",
            banded,bare,noRig,plain,plainOld,expected);json+=levelText;
        result("eyelid_band",bare>.05 && std::abs(banded/bare/expected-1)<.01,float(banded/std::max(bare,1e-9)));
        result("eyelid_band_off_unchanged",bare>.05 && noRig==bare && plain==plainOld,float(noRig/std::max(bare,1e-9)));
    }
    // 0.23.1 (docs\pt-cover-guides-20261104): a layer whose coverage varies with the seed is in the guides in the share
    // of frames the path takes it - .3 coverage over an opaque surface guides .3 layer + .7 surface, the same for every
    // seed, its instance the larger share's; bit 27 cleared, the layer above coverage .5 alone (0.23.0: the surface).
    {
        const uint32_t all=0xffffffffu,old=all&~(1u<<27);
        for(unsigned i=0;i<2;i++){geo[i].flags=rtc::kRigid;geo[i].indexBuffer=ib.Get();geo[i].submeshes=&sub[i?1:0];geo[i].matrix[11]=-.1f*i;}
        frame.geometryCount=2;frame.lightCount=0;view.samples=1;view.maxBounces=1;rtc::SetPathEnvironment(rtc::PathEnvironment{});
        opaque=rtc::PathMaterial{};opaque.roughness=1;for(int c=0;c<3;c++){opaque.baseColor[c]=.8f;opaque.specular[c]=0;}
        struct Guide{float diffuse,instance;};
        const float texels[4][4]={{1,1,1,1},{1,1,1,0},{1,1,1,.3f},{1,1,1,.7f}};
        decltype(FixtureTexture(1,texels[0])) maps[4];for(int k=0;k<4;k++)maps[k]=FixtureTexture(1,texels[k]);
        auto coverGuide=[&](int coverage,unsigned seed,uint32_t layers) {
            layer=rtc::PathMaterial{};layer.roughness=1;for(int c=0;c<3;c++){layer.baseColor[c]=.1f;layer.specular[c]=0;}
            layer.alpha=maps[coverage].Get();view.seed=seed;rtc::SetTestLayers(layers);
            if(!render())return Guide{-1,-1};
            auto pixels=ReadTexture(view.diffuse,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            return Guide{Half(pixels[center]),Half(pixels[center+3])};
        };
        Guide cloth=coverGuide(0,7,all),surface=coverGuide(1,7,all),sheer=coverGuide(2,7,all),sheer2=coverGuide(2,43,all),
              sheerOld=coverGuide(2,7,old),dense=coverGuide(3,43,all);
        rtc::SetTestLayers(all);layer=rtc::PathMaterial{};frame.geometryCount=1;
        const float expectSheer=.3f*cloth.diffuse+.7f*surface.diffuse,expectDense=.7f*cloth.diffuse+.3f*surface.diffuse;
        char levelText[320];snprintf(levelText,sizeof levelText,",\"coverage_guide_levels\":{\"cloth\":%.6g,\"surface\":%.6g,\"sheer\":%.6g,\"sheer_seed\":%.6g,\"sheer_old\":%.6g,\"dense\":%.6g,\"sheer_instance\":%g,\"dense_instance\":%g}",
            cloth.diffuse,surface.diffuse,sheer.diffuse,sheer2.diffuse,sheerOld.diffuse,dense.diffuse,sheer.instance,dense.instance);json+=levelText;
        result("coverage_guides",std::abs(cloth.diffuse-surface.diffuse)>.3f && std::abs(sheer.diffuse-expectSheer)<.004f &&
            std::abs(dense.diffuse-expectDense)<.004f && sheer.diffuse==sheer2.diffuse && sheer.instance==1 && dense.instance==2,
            sheer.diffuse/std::max(expectSheer,1e-6f));
        result("coverage_guides_off",sheerOld.diffuse==surface.diffuse && sheerOld.instance==1,sheerOld.diffuse);
    }
    // 11-05: with bit 29 a see-through layer has no depth or motion of
    // its own in the guides (the deepest layer's), but a person's own layer (PathMaterial::mode 256) keeps them: .3
    // coverage cloth guides the cloth's depth when it is a person's, the surface's when it is not or bit 30 is cleared.
    {
        const uint32_t all=0xffffffffu,old=all&~(1u<<30);
        for(unsigned i=0;i<2;i++){geo[i].flags=rtc::kRigid;geo[i].indexBuffer=ib.Get();geo[i].submeshes=&sub[i?1:0];geo[i].matrix[11]=-.1f*i;}
        frame.geometryCount=2;frame.lightCount=0;view.samples=1;view.maxBounces=1;rtc::SetPathEnvironment(rtc::PathEnvironment{});
        opaque=rtc::PathMaterial{};opaque.roughness=1;for(int c=0;c<3;c++){opaque.baseColor[c]=.8f;opaque.specular[c]=0;}
        const float texels[3][4]={{1,1,1,1},{1,1,1,0},{1,1,1,.3f}};
        decltype(FixtureTexture(1,texels[0])) maps[3];for(int k=0;k<3;k++)maps[k]=FixtureTexture(1,texels[k]);
        auto layerDepth=[&](int coverage,uint32_t mode,unsigned seed,uint32_t layers) {
            layer=rtc::PathMaterial{};layer.roughness=1;layer.mode=mode;for(int c=0;c<3;c++){layer.baseColor[c]=.1f;layer.specular[c]=0;}
            layer.alpha=maps[coverage].Get();view.seed=seed;rtc::SetTestLayers(layers);
            if(!render())return -1.f;
            return Half(ReadTexture(view.depth,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1)[center/4]);
        };
        const float cloth=layerDepth(0,256,7,all),surface=layerDepth(1,256,7,all),person=layerDepth(2,256,7,all),
                    person2=layerDepth(2,256,43,all),other=layerDepth(2,0,7,all),off=layerDepth(2,256,7,old);
        rtc::SetTestLayers(all);layer=rtc::PathMaterial{};frame.geometryCount=1;
        char levelText[256];snprintf(levelText,sizeof levelText,",\"person_guide_depths\":{\"cloth\":%.6g,\"surface\":%.6g,\"person\":%.6g,\"person_seed\":%.6g,\"not_person\":%.6g,\"bit30_off\":%.6g}",
            cloth,surface,person,person2,other,off);json+=levelText;
        result("person_layer_guides",cloth>0 && surface>0 && cloth!=surface && person==cloth && person2==cloth,person);
        result("person_layer_guides_off",other==surface && off==surface,off);
    }
    // Shadow terminator (pt-terminator-20261102): a coarse sphere with smooth
    // vertex normals under a point-like side light. Where the shading normal
    // faces the light the facets must not cut the light off: count sphere
    // pixels with N.L > .2 that come out below a quarter of Lambert. Camera at
    // z -3 looking down +z (probe.cpp).
    {
        constexpr int segments=10,rings=6;std::vector<float> sp,sn;std::vector<uint32_t> si;
        for(int r=0;r<=rings;r++)for(int k=0;k<=segments;k++) {
            float th=3.14159265f*r/rings,ph=6.28318531f*k/segments;
            float n[3]={std::sin(th)*std::cos(ph),std::cos(th),std::sin(th)*std::sin(ph)};
            sn.insert(sn.end(),n,n+3);sp.push_back(n[0]);sp.push_back(n[1]);sp.push_back(n[2]-1.5f);
        }
        for(int r=0;r<rings;r++)for(int k=0;k<segments;k++) {
            uint32_t a=r*(segments+1)+k,b=a+segments+1;
            uint32_t tri[6]={a,a+1,b,a+1,b+1,b};si.insert(si.end(),tri,tri+6);
        }
        auto svb=Data(sp.data(),sp.size()*4),snb=Data(sn.data(),sn.size()*4),sib=Data(si.data(),si.size()*4);
        rtc::Submesh ssub;ssub.count=(uint32_t)si.size();ssub.material=sub[0].material;
        auto &g=geo[0];g=rtc::Geometry{};g.id=390;g.flags=rtc::kRigid;g.vertexCount=(uint32_t)(sp.size()/3);
        g.position={svb.Get(),0,12,rtc::VertexFormat::Float3};g.normal={snb.Get(),0,12,rtc::VertexFormat::Float3};
        g.indexBuffer=sib.Get();g.index32=1;g.submeshCount=1;g.submeshes=&ssub;
        const float m[12]={1,0,0,0,0,1,0,0,0,0,1,0};memcpy(g.matrix,m,sizeof m);
        rtc::Light side;side.id=702;side.type=rtc::LightType::Directional;side.angularRadius=0;for(auto &v:side.color)v=1;
        const float travel[3]={-.8f,-.3f,.52f};const float len=std::sqrt(travel[0]*travel[0]+travel[1]*travel[1]+travel[2]*travel[2]);
        for(int c=0;c<3;c++)side.direction[c]=travel[c]/len;
        frame.geometryCount=1;frame.lights=&side;frame.lightCount=1;view.samples=1;view.maxBounces=1;
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;
        auto colour=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4),normals=ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
        auto depth=ReadTexture(view.depth,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1);
        // Silhouette pixels whose shading normal turns from the camera are a
        // different artefact (the view-side terminator) and are left out.
        auto nv=[&](int i){float x=((i%128)+.5f)/64-1,y=1-((i/128)+.5f)/64,l=std::sqrt(x*x+y*y+1);
            return -(Half(normals[i*4])*x+Half(normals[i*4+1])*y+Half(normals[i*4+2]))/l;};
        std::vector<float> ratio;unsigned facing=0,cut=0;
        for(int i=0;i<128*128;i++) {
            if(!(Half(depth[i])>0) || nv(i)<.1f)continue;
            float nl=0;for(int c=0;c<3;c++)nl-=Half(normals[i*4+c])*side.direction[c];
            if(nl>.5f)ratio.push_back(Half(colour[i*4])/nl);
        }
        std::sort(ratio.begin(),ratio.end());float k=ratio.empty()?0:ratio[ratio.size()/2];
        for(int i=0;i<128*128;i++) {
            if(!(Half(depth[i])>0) || nv(i)<.1f)continue;
            float nl=0;for(int c=0;c<3;c++)nl-=Half(normals[i*4+c])*side.direction[c];
            if(nl>.2f){facing++;cut+=Half(colour[i*4])<.25f*k*nl;}
        }
        float share=facing?float(cut)/facing:1;
        char text[160];snprintf(text,sizeof text,",\"terminator_counts\":{\"facing\":%u,\"cut\":%u,\"lambert_scale\":%.6g}",facing,cut,k);json+=text;
        result("shadow_terminator_smooth",facing>500 && k>0 && share<.001f,share);
        frame.lightCount=0;
    }
    // Author rules (11-09, docs\pt-author-intent-20261109). What VaM's authors see in Unity, as the PT reads it.
    {
        // geo[0] is still the terminator's sphere: planes at z 1 again, the camera at z -3 with tan 1 (probe.cpp).
        const float flat[12]={1,0,0,0,0,1,0,0,0,0,1,0};
        auto plane=[&](rtc::Geometry &g,uint64_t id,rtc::Submesh *s,float z,uint32_t flags) {
            g=rtc::Geometry{};g.id=id;g.flags=flags;g.vertexCount=4;g.position={vb.Get(),0,12,rtc::VertexFormat::Float3};g.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
            g.normal={nb.Get(),0,12,rtc::VertexFormat::Float3};g.indexBuffer=ib.Get();g.index32=1;g.submeshCount=1;g.submeshes=s;
            memcpy(g.matrix,flat,sizeof flat);g.matrix[11]=z-1;
        };
        rtc::Geometry planes[3];plane(planes[0],391,&sub[0],1,rtc::kRigid);frame.geometries=planes;
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        layer=rtc::PathMaterial{};
        // Unity's spot at the camera, half cone tan 1, on the plane 4 m ahead: r is the pixel's offset from the centre (measured
        // in the game, measure-121324): its cookie x 1/(1+25 (d/range)^2) x N.L.
        rtc::Light spot;spot.id=710;spot.type=rtc::LightType::Spot;spot.position[2]=-3;spot.direction[2]=1;spot.range=10;spot.spotCosOuter=std::sqrt(.5f);
        spot.radius=0;spot.angularRadius=0;for(auto &v:spot.color)v=1;
        rtc::PathEnvironment author;author.authorRules=rtc::kPathAuthorSpot;rtc::SetPathEnvironment(author);
        frame.geometryCount=1;frame.lights=&spot;frame.lightCount=1;view.samples=8;view.maxBounces=1;
        if(!render())return false;
        auto spotColour=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
        auto spotAt=[&](int col){return Half(spotColour[(64*128+col)*4]);};
        auto spotWant=[&](int col,float cookie) {
            float x=(col+.5f)/64-1,y=1-64.5f/64,d2=1+x*x+y*y;
            return cookie/std::sqrt(d2)/(1+25*.16f*d2);
        };
        const int c0=64,c1=64+32,c2=64+52;  // r .008, .5 (cookie 1), .82 (the texture's .518)
        float scale=spotAt(c0)/spotWant(c0,1),mid=spotAt(c1)/(scale*spotWant(c1,1)),rim=spotAt(c2)/(scale*spotWant(c2,.52f));
        result("author_spot_inside",scale>0 && std::abs(mid-1)<.02f,scale>0?mid:0);
        result("author_spot_cookie",scale>0 && std::abs(rim-1)<.06f,scale>0?rim:0);
        result("author_spot_past_cone",scale>0 && spotAt(64+63)<.01f*spotAt(c0),spotAt(64+63));
        // Unity's shadow: nothing inside its near plane casts; a shadowless light keeps only contact shadows.
        rtc::Light lamp;lamp.id=711;lamp.type=rtc::LightType::Point;lamp.position[2]=.5f;lamp.radius=0;lamp.angularRadius=0;lamp.range=100;for(auto &v:lamp.color)v=1;
        lamp.shadowNear=.3f;plane(planes[1],392,&sub[0],.7f,rtc::kRigid|rtc::kCameraHidden);frame.geometryCount=2;frame.lights=&lamp;  // .2 m from the lamp, inside its near plane
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;float nearOff=region(view.color,0);
        author=rtc::PathEnvironment{};author.authorRules=rtc::kPathAuthorNear;rtc::SetPathEnvironment(author);
        if(!render())return false;float nearOn=region(view.color,0);
        result("author_near_plane",nearOff<1e-4f && nearOn>.01f,nearOn);
        lamp.shadowNear=0;lamp.unityShadows=0;lamp.position[2]=-5;plane(planes[1],393,&sub[0],-1,rtc::kRigid|rtc::kCameraHidden);  // 2 m from the receiver
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;float farOff=region(view.color,0);
        author=rtc::PathEnvironment{};author.authorRules=rtc::kPathAuthorNoShadow;author.authorContact=.6f;rtc::SetPathEnvironment(author);
        if(!render())return false;float farOn=region(view.color,0);
        plane(planes[1],394,&sub[0],.8f,rtc::kRigid|rtc::kCameraHidden);  // .2 m from the receiver: the contact shadow stays
        if(!render())return false;float contactOn=region(view.color,0);
        result("author_no_shadow_far",farOff<1e-4f && farOn>.01f,farOn);
        result("author_no_shadow_contact",contactOn<1e-4f,contactOn);
        // Skybox/Panoramic: Unity draws the sphere first and without depth. What lies beyond it covers it; alone, it shows.
        rtc::MaterialDesc panoDesc;rtc::Submesh panoSub;panoSub.count=6;panoSub.material=rtc::CreateMaterial(panoDesc);
        rtc::PathMaterial pano;pano.flags=1024;pano.mode=2048;for(auto &v:pano.emissionColor)v=.25f;
        if(!rtc::SetPathMaterial(panoSub.material,pano))return false;
        opaque=rtc::PathMaterial{};opaque.flags=1024;for(auto &v:opaque.emissionColor)v=1;
        plane(planes[1],395,&panoSub,.6f,rtc::kRigid);
        frame.lightCount=0;frame.geometryCount=2;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;float panoOff=region(view.color,0);
        author=rtc::PathEnvironment{};author.authorRules=rtc::kPathAuthorPanorama;rtc::SetPathEnvironment(author);
        if(!render())return false;float panoBehind=region(view.color,0);
        frame.geometries=&planes[1];frame.geometryCount=1;
        if(!render())return false;float panoAlone=region(view.color,0);
        float panoDepth=Half(ReadTexture(view.depth,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1)[64*128+64]);
        frame.geometries=planes;
        result("author_panorama_rule_off",std::abs(panoOff-.25f)<.01f,panoOff);
        result("author_panorama_covered",std::abs(panoBehind-1)<.01f,panoBehind);
        result("author_panorama_alone",std::abs(panoAlone-.25f)<.01f,panoAlone);
        result("author_panorama_no_depth",panoDepth==0,panoDepth);
        // PluginIdea's mirror sheet: its reflection x _ReflectStrengh x (_Freshnel (1 + dir.y)^5 + .1) over what is behind, undimmed.
        // What it reflects: an emitter behind the camera.
        rtc::MaterialDesc sheetDesc;sheetDesc.kind=rtc::MaterialKind::Translucent;rtc::Submesh sheetSub;sheetSub.count=6;sheetSub.material=rtc::CreateMaterial(sheetDesc);
        rtc::PathMaterial sheet;sheet.flags=2048;sheet.transmission=1;sheet.mode=131072;sheet.materialControls[1]=.5f;sheet.directGain[3]=.5f;
        for(auto &v:sheet.specular)v=0;
        if(!rtc::SetPathMaterial(sheetSub.material,sheet))return false;
        rtc::MaterialDesc targetDesc;rtc::Submesh targetSub;targetSub.count=6;targetSub.material=rtc::CreateMaterial(targetDesc);
        rtc::PathMaterial target;target.flags=1024;for(auto &v:target.emissionColor)v=.2f;
        if(!rtc::SetPathMaterial(targetSub.material,target))return false;
        plane(planes[1],396,&sheetSub,.6f,rtc::kRigid);plane(planes[2],397,&targetSub,-4,rtc::kRigid);
        frame.geometryCount=3;view.samples=8;view.maxBounces=2;
        for(auto &v:opaque.emissionColor)v=.5f;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});
        if(!render())return false;float sheetRed=region(view.color,0);
        result("author_mirror_sheet",std::abs(sheetRed-(.5f+.2f*.3f))<.03f,sheetRed);  // head on: .5 x (.5 + .1) of the emitter's .2 over the .5 behind
        // Skyshop's diffuse in Unity is its SH x _ExposureIBL.x; the cube is for reflections only (one scene: a bright HDR probe under
        // a reflection scale of .0027). Rule off: the cube as ever, the SH ignored. Rule on: the SH (albedo .5 x .4), no cube.
        opaque=rtc::PathMaterial{};for(int c=0;c<3;c++){opaque.baseColor[c]=.5f;opaque.specular[c]=0;}opaque.roughness=1;opaque.materialControls[1]=0;
        layer=rtc::PathMaterial{};frame.geometries=planes;frame.geometryCount=1;frame.lightCount=0;view.samples=8;view.maxBounces=1;
        rtc::PathEnvironment skySh;skySh.lighting=cube.Get();skySh.rgbm=0;skySh.lightingScale=0;skySh.diffuseScale=1;rtc::SetPathEnvironment(skySh);
        if(!render())return false;float cubeAlone=region(view.color,0);
        frame.skySh[3]=frame.skySh[7]=frame.skySh[11]=.4f;
        if(!render())return false;float cubeWithSh=region(view.color,0);
        skySh.authorRules=rtc::kPathAuthorSkySh;rtc::SetPathEnvironment(skySh);
        if(!render())return false;float shOn=region(view.color,0);
        frame.skySh[3]=frame.skySh[7]=frame.skySh[11]=0;
        if(!render())return false;float shNone=region(view.color,0);
        result("author_sky_sh_off_keeps_cube",cubeAlone>.1f && std::abs(cubeWithSh/cubeAlone-1)<.03f,cubeWithSh);
        result("author_sky_sh_on",std::abs(shOn-.2f)<.01f,shOn);
        result("author_sky_sh_no_cube",shNone<1e-4f,shNone);
        // Under an open sky the broad reflection read from the cube is the estimate a ray that escapes makes: the same mean.
        for(int c=0;c<3;c++){opaque.baseColor[c]=.2f;opaque.specular[c]=.5f;}opaque.roughness=.6f;opaque.materialControls[1]=1;frame.skySh[3]=frame.skySh[7]=frame.skySh[11]=0;
        rtc::PathEnvironment rough;rough.lighting=cube.Get();rough.rgbm=0;rough.lightingScale=1;rough.diffuseScale=1;view.maxBounces=2;
        auto roughMean=[&](uint32_t rules){rough.authorRules=rules;rtc::SetPathEnvironment(rough);double sum=0;for(unsigned seed=0;seed<16;seed++){view.seed=4000+seed;if(!render())return -1.f;sum+=region(view.color,0);}return float(sum/16);};
        float traced=roughMean(rtc::kPathAuthorBgBounce),cubeRead=roughMean(rtc::kPathAuthorBgBounce|rtc::kPathAuthorBgRough);
        result("author_bg_rough_open_sky",traced>.05f && std::abs(cubeRead/traced-1)<.03f,cubeRead/std::max(traced,1e-6f));
        view.samples=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});view.samples=1;view.maxBounces=1;frame.lightCount=0;frame.geometries=geo;frame.geometryCount=0;
    }
    // The skin's scattering in screen space (11-10, docs\pt-skin-sss-ss-20261110): a hidden half-plane shades the right half
    // of a skin plane from a frontal sun (columns 64 on). With a scattering strength the direct diffuse light is spread over
    // the skin profile (.05 m a profile millimetre: its widest Gaussian 2.2 px here): red reaches into the shadow, blue
    // hardly; far from the edge nothing changes; along the row the light is moved, not made.
    {
        const float flat[12]={1,0,0,0,0,1,0,0,0,0,1,0},shade[12]={1,0,0,10,0,1,0,0,0,0,1,-1};
        rtc::Geometry pair[2];
        for(int i=0;i<2;i++) {
            auto &g=pair[i];g=rtc::Geometry{};g.id=398+i;g.flags=i?rtc::kRigid|rtc::kCameraHidden:rtc::kRigid;g.vertexCount=4;
            g.position={vb.Get(),0,12,rtc::VertexFormat::Float3};g.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
            g.normal={nb.Get(),0,12,rtc::VertexFormat::Float3};g.indexBuffer=ib.Get();g.index32=1;g.submeshCount=1;g.submeshes=&sub[0];
            memcpy(g.matrix,i?shade:flat,sizeof flat);
        }
        rtc::Light sun;sun.id=712;sun.type=rtc::LightType::Directional;sun.direction[2]=1;sun.radius=0;sun.angularRadius=0;for(auto &v:sun.color)v=1;
        frame.geometries=pair;frame.geometryCount=2;frame.lights=&sun;frame.lightCount=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});view.samples=1;view.maxBounces=1;view.seed=11;
        opaque=rtc::PathMaterial{};opaque.flags=8;for(int c=0;c<3;c++)opaque.baseColor[c]=.8f;
        opaque.skin[0]=.028f;for(int i=1;i<=5;i++)opaque.skin[i]=1;opaque.skin[20]=.05f;
        layer=rtc::PathMaterial{};
        std::vector<float> rows[2];
        for(int on=0;on<2;on++) {
            opaque.skin[21]=(float)on;if(!render())return false;
            auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(int x=0;x<128;x++)for(int c=0;c<3;c++)rows[on].push_back(Half(pixels[(64*128+x)*4+c]));
        }
        auto at=[&](int on,int x,int c){return rows[on][x*3+c];};
        const float litOff=at(0,16,0),litOn=at(1,16,0),darkOff=at(0,112,0),darkOn=at(1,112,0);
        // Moved, not made: 32 px either side of the edge (5x the widest tap). Over the whole row the image's borders count
        // too: there taps off the image count nothing and the rest renormalise (as in the raster route), so a gradient
        // that meets the border shifts a little; reported, not tested.
        double red=0,blue=0,moved=0,net=0,rowNet=0;
        for(int x=64;x<80;x++){red+=at(1,x,0)-at(0,x,0);blue+=at(1,x,2)-at(0,x,2);}
        for(int x=0;x<128;x++){const double d=at(1,x,0)-at(0,x,0);rowNet+=d;if(x>=32 && x<96){net+=d;moved+=std::abs(d);}}
        char text[320];snprintf(text,sizeof text,",\"sss_edge_row\":{\"lit\":%.6g,\"edge_off\":[%.6g,%.6g,%.6g],\"edge_on\":[%.6g,%.6g,%.6g],\"red_in\":%.6g,\"blue_in\":%.6g,\"window_net\":%.6g,\"window_moved\":%.6g,\"row_net\":%.6g}",
            litOff,at(0,64,0),at(0,64,1),at(0,64,2),at(1,64,0),at(1,64,1),at(1,64,2),red,blue,net,moved,rowNet);
        json+=text;
        result("sss_far_unchanged",litOff>.05f && std::abs(litOn/litOff-1)<.01f && std::abs(darkOn-darkOff)<1e-3f,litOn/std::max(litOff,1e-6f));
        result("sss_red_into_shadow",red>.01 && red>4*std::max(blue,0.0),(float)red);
        result("sss_light_moved_not_made",moved>.01 && std::abs(net)<.05*moved,(float)(net/std::max(moved,1e-9)));
        opaque.skin[21]=0;rtc::SetPathMaterial(sub[0].material,opaque); // later checks trace without scattering, as before
        frame.lightCount=0;frame.geometries=geo;frame.geometryCount=0;
    }
    // Light through thin skin (11-10, docs\thin-light-20261110): the eye's skin plane (z 1) and a second one behind it (z 1 + t,
    // hidden from the camera) make a slab; a sun behind it, travelling toward the camera. Head on, the eye's skin has only
    // the light that came through: .8 (albedo) x strength x exp(-(t/sigma)^2/2) per colour, sigma 2.5 / 1.45 / .85 mm:
    // 3 mm .3894 / .0941 / .0016. 15 mm, strength 0, or a plane behind the slab: nothing. A sun in front: as without.
    {
        const float flat[12]={1,0,0,0,0,1,0,0,0,0,1,0};
        rtc::Geometry slab[3];
        for(int i=0;i<3;i++) {
            auto &g=slab[i];g=rtc::Geometry{};g.id=400+i;g.flags=i?rtc::kRigid|rtc::kCameraHidden:rtc::kRigid;g.vertexCount=4;
            g.position={vb.Get(),0,12,rtc::VertexFormat::Float3};g.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
            g.normal={nb.Get(),0,12,rtc::VertexFormat::Float3};g.indexBuffer=ib.Get();g.index32=1;g.submeshCount=1;g.submeshes=&sub[0];
            memcpy(g.matrix,flat,sizeof flat);
        }
        rtc::Light sun;sun.id=713;sun.type=rtc::LightType::Directional;sun.direction[2]=-1;sun.radius=0;sun.angularRadius=0;for(auto &v:sun.color)v=1;
        frame.geometries=slab;frame.lights=&sun;frame.lightCount=1;
        rtc::SetPathEnvironment(rtc::PathEnvironment{});view.samples=1;view.maxBounces=1;view.seed=12;
        opaque=rtc::PathMaterial{};opaque.flags=8;for(int c=0;c<3;c++)opaque.baseColor[c]=.8f;
        opaque.skin[0]=.028f;for(int i=1;i<=5;i++)opaque.skin[i]=1;
        layer=rtc::PathMaterial{};
        auto shot=[&](float t,float strength,int planes,float sunZ,float rgb[3]) {
            slab[1].matrix[11]=t;slab[2].matrix[11]=t+.005f;frame.geometryCount=planes;sun.direction[2]=sunZ;opaque.skin[22]=strength;
            if(!render())return false;
            auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(int c=0;c<3;c++)rgb[c]=Half(pixels[center+c]);
            return true;
        };
        float thin[3],thick[3],off[3],blocked[3],frontOn[3],frontOff[3];
        if(!shot(.003f,1,2,-1,thin) || !shot(.015f,1,2,-1,thick) || !shot(.003f,0,2,-1,off) || !shot(.003f,1,3,-1,blocked) ||
           !shot(.003f,1,2,1,frontOn) || !shot(.003f,0,2,1,frontOff))return false;
        char text[320];snprintf(text,sizeof text,",\"thin_light_rows\":{\"thin\":[%.6g,%.6g,%.6g],\"thick\":%.6g,\"off\":%.6g,\"blocked\":%.6g,\"front\":[%.6g,%.6g]}",
            thin[0],thin[1],thin[2],thick[0],off[0],blocked[0],frontOn[0],frontOff[0]);
        json+=text;
        const float expect[3]={.8f*expf(-.5f*1.2f*1.2f),.8f*expf(-.5f*(3/1.45f)*(3/1.45f)),.8f*expf(-.5f*(3/.85f)*(3/.85f))};
        result("thin_light_3mm_red",std::abs(thin[0]/expect[0]-1)<.01f,thin[0]/expect[0]);
        result("thin_light_3mm_green",std::abs(thin[1]/expect[1]-1)<.02f,thin[1]/expect[1]);
        result("thin_light_3mm_blue",std::abs(thin[2]-expect[2])<1e-3f,thin[2]);
        result("thin_light_15mm_none",thick[0]<1e-4f,thick[0]);
        result("thin_light_strength_0_none",off[0]<1e-5f,off[0]);
        result("thin_light_blocked_behind",blocked[0]<1e-5f,blocked[0]);
        result("thin_light_front_unchanged",frontOff[0]>.05f && frontOn[0]==frontOff[0] && frontOn[1]==frontOff[1] && frontOn[2]==frontOff[2],frontOn[0]);
        opaque.skin[22]=0;rtc::SetPathMaterial(sub[0].material,opaque);
        frame.lightCount=0;frame.geometries=geo;frame.geometryCount=0;
    }
    json+="}";FILE *f=fopen((out+"/contracts.json").c_str(),"wb");if(f){fputs(json.c_str(),f);fclose(f);}
    puts(json.c_str());return pass;
}
