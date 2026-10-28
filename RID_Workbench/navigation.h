#pragma once
// One coordinate link; the browser/OS resolves the map handler on a user click.
// No external request, app probing, or automatic fallback navigation.
static const char kNavigationScript[] PROGMEM = R"JS(
'use strict';
globalThis.RIDMaps=Object.freeze({
  links(lat,lon,crs,label,android=false){
    if(typeof lat!=='number'||typeof lon!=='number'||!Number.isFinite(lat)||!Number.isFinite(lon)||Math.abs(lat)>90||Math.abs(lon)>180||!['WGS-84','CGCS2000'].includes(crs))return null;
    const point=lat.toFixed(7)+','+lon.toFixed(7);
    const href=android?'geo:'+point+'?q='+point+'('+encodeURIComponent(label||'RID location')+')':'https://maps.apple.com/directions?destination='+point;
    return {href,approximate:crs==='CGCS2000'};
  }
});
)JS";
