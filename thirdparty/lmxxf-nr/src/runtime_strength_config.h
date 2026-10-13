#pragma once
#include <cmath>
#include <cwchar>
#include <cwctype>
namespace runtime_strength {
enum class State { Inherit, Numeric, Invalid };
struct Parsed { State state=State::Inherit;float transfer=1.f,color=1.f; };
inline Parsed Parse(const wchar_t*text){
 if(!text)return {};while(std::iswspace(*text))++text;if(!*text)return {};
 if(!std::wcsncmp(text,L"auto",4)){auto tail=text+4;while(std::iswspace(*tail))++tail;if(!*tail)return {};}
 float a=0,b=0;int end=0;
 if(std::swscanf(text,L"%f , %f %n",&a,&b,&end)==2&&end>0&&!text[end]&&std::isfinite(a)&&std::isfinite(b)&&a>=0&&a<=1&&b>=0&&b<=1)return {State::Numeric,a,b};
 return {State::Invalid,1.f,1.f};
}
inline void Override(const Parsed&p,float&a,float&b){if(p.state==State::Numeric){a=p.transfer;b=p.color;}}
}
