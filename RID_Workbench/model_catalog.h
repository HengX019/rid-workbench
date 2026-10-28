#pragma once
#include "rid_decoder.h"
#include "model_codes.h"
#include <cstring>
#include <cstddef>
namespace rid {
// Self-ID aliases and the separately sourced serial catalog do not restrict
// which aircraft can be received.
struct ModelName {const char *name;const char *key;};
constexpr ModelName MainlandModels[]={
 {"antigravitya1","antigravitya1"},{"mavic4pro","djimavic4pro"},
 {"air3s","djiair3s"},{"air3","djiair3"},
 {"mini5pro","djimini5pro"},{"mini4pro","djimini4pro"},
 {"lito1","djilito1"},{"litox1","djilitox1"},
 {"neo","djineo"},{"neo2","djineo2"},
 {"flip","djiflip"},{"avata2","djiavata2"},
 {"mavic3pro","djimavic3pro"},{"mini3pro","djimini3pro"},
 {"evolite","autelevolite"},{"evoiipro","autelevoiipro"},
 {"avata360","djiavata360"},{"mavic3","djimavic3"},
 {"mavic3classic","djimavic3classic"},{"air2s","djiair2s"},{"avata","djiavata"},
 {"mavic3cine","djimavic3cine"},{"mavic3procine","djimavic3procine"},{"mini3","djimini3"}
};
inline const char *declaredModelName(const char *text){
 if(!strcmp(text,"影翎A1")||!strcmp(text,"影翎 A1")||!strcmp(text,"影翎Antigravity A1")||!strcmp(text,"影翎 Antigravity A1"))return "antigravitya1";
 if(!strcmp(text,"御4Pro")||!strcmp(text,"御4 Pro")||!strcmp(text,"大疆御4Pro"))return "mavic4pro";
 char key[48]{};size_t n=0;
 for(const unsigned char *p=(const unsigned char *)text;*p;++p){
  if(*p==' '||*p=='-'||*p=='_')continue;
  if(*p>=128 || n==sizeof(key)-1)return nullptr;
  key[n++]=*p>='A'&&*p<='Z'?*p+32:*p;
 }
 for(const auto &entry:SerialModels){
  if(entry.model[0] && (!strcmp(key,entry.model) || (!strncmp(key,"dji",3) && !strcmp(key+3,entry.model))))return entry.model;
 }
 for(const auto &m:MainlandModels){
  if(!strcmp(key,m.key))return m.name;
  if(!strcmp(key,m.name))return m.name;
 }
 return nullptr;
}
inline const SerialModel *serialModel(const Device &d){
 // ASTM registration/session IDs are not aircraft factory serials. GB field
 // 001 is the product identity. Never match only the four-character model code.
 if((!d.protocol && d.idType!=1) || strlen(d.uasId)!=20)return nullptr;
 for(size_t i=0;i<20;++i){const char c=d.uasId[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')))return nullptr;}
 for(const auto &entry:SerialModels)if(!strncmp(d.uasId,entry.prefix,8))return &entry;
 return nullptr;
}
struct ProductCode {bool valid=false;char manufacturer[5]{},model[5]{},serial[13]{};};
inline ProductCode productCode(const Device &d){
 ProductCode out;
 // GB field 001 identifies the GB/T 41300 product code. A legacy serial or
 // an ASTM Basic ID has no guaranteed domestic 4+4+12 semantics.
 if(!d.protocol || strlen(d.uasId)!=20)return out;
 for(size_t i=0;i<20;++i){const char c=d.uasId[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z'&&c!='I'&&c!='O')))return out;}
 memcpy(out.manufacturer,d.uasId,4);memcpy(out.model,d.uasId+4,4);memcpy(out.serial,d.uasId+8,12);out.valid=true;return out;
}
}
