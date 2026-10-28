#include "document.h"
#include "scene_view.h"
#include "usd/loader.h"
#include <iostream>

int main(int argc,char **argv) {
  try {
    for(int i=1;i<argc;++i) {
      auto stage=pxr::UsdStage::Open(argv[i]);
      if(!stage)throw std::runtime_error("Cannot open fixture");
      auto toolkit=editor::compose_scene(stage,1);
      // The loader reports expanded per-instance geometry.
      for(auto &mesh:toolkit["meshes"])mesh=editor::expanded_mesh(toolkit,mesh);
      toolkit.erase("prototypes");
      auto direct=remix::usd::load({argv[i]}).scene;
      for(const char *group:{"meshes","lights","cameras","materials"}) {
        auto &actual=direct[group];
        for(auto &item:actual)item.erase("hash");
        for(const auto &expected:toolkit[group]) {
          auto found=std::find_if(actual.begin(),actual.end(),[&](const auto &item){return item.at("id")==expected.at("id");});
          if(found==actual.end() || *found!=expected) {
            auto difference=found==actual.end()?editor::Json("missing"):editor::Json::diff(expected,*found);
            throw std::runtime_error(std::string(group)+" "+expected.at("id").template get<std::string>()+": "+difference.dump().substr(0,4096));
          }
        }
        if(std::string(group)!="materials" && toolkit[group].size()!=actual.size())throw std::runtime_error("Object count differs");
      }
      std::cout<<argv[i]<<": composed transforms, units, attributes, geometry, materials, lights and cameras match ("<<toolkit["meshes"].size()<<" meshes)\n";
    }
  }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
