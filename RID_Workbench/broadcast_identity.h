#pragma once
#include "rid_decoder.h"
#include "simulation.h"
#include "model_catalog.h"
namespace rid {
struct BroadcastIdentity { const char *model; const char *source;const SerialModel *catalog=nullptr;bool conflict=false; };
inline BroadcastIdentity declaredBroadcastIdentity(const Device &d) {
  const char *text=d.selfDescription;
  if(!d.selfIdReceived || !text[0])return {"","not_provided"};
  if((!strncmp(d.uasId,"SIM-",4) || (strlen(d.uasId)==20 && d.uasId[8]=='I')) && !strncmp(text,"SIM|",4) && text[4]) {
    const char *name=declaredModelName(text+4);
    return {name?name:text+4,"simulation_declared"};
  }
  // Self-ID is free text, not a mandatory product-model field. Only an
  // explicit model declaration is used as the title; always expose raw text.
  const char *prefixes[]={"MODEL=","MODEL:","Model=","Model:","model=","model:","型号:","型号："};
  for(auto prefix:prefixes) {
    const size_t n=strlen(prefix);
    if(!strncmp(text,prefix,n)) {
      const char *model=text+n;while(*model==' ')++model;
      if(*model){const char *name=declaredModelName(model);return {name?name:model,"broadcast_declared"};}
    }
  }
  if(const char *name=declaredModelName(text))return {name,"broadcast_declared"};
  // A plain, exact product name in Self-ID is also a broadcast declaration.
  for(auto name:lab::SimModels)if(!strcmp(text,name))return {text,"broadcast_declared"};
  return {"","not_provided"};
}
inline BroadcastIdentity broadcastIdentity(const Device &d){
 auto identity=declaredBroadcastIdentity(d);
 identity.catalog=serialModel(d);
 if(identity.catalog && identity.catalog->model[0]){
   if(!identity.model[0]){identity.model=identity.catalog->model;identity.source="serial_catalog";}
   else identity.conflict=strcmp(identity.model,identity.catalog->model)!=0;
 }
 if(identity.catalog && !identity.catalog->model[0] && !identity.model[0])identity.source="serial_catalog_ambiguous";
 return identity;
}
}
