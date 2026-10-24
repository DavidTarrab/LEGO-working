#include "LegoUpstreamModel.h"
#include "Modules/ModuleManager.h"
class FLegoUpstreamModule : public FDefaultModuleImpl
{
    void ShutdownModule() override { FLegoUpstreamModel::GetInstance().Exit(); }
};
IMPLEMENT_MODULE(FLegoUpstreamModule, LegoUpstreamModel);
