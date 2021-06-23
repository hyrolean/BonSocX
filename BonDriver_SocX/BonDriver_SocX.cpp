// BonDriver_SocX.cpp : DLL アプリケーション用にエクスポートされる関数を定義します。
//

#include "stdafx.h"

#include "BonTuner.h"

#pragma warning( disable : 4273 )

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	return BonTuner!=NULL ? BonTuner : (BonTuner=new CBonTuner, BonTuner) ;
}

#pragma warning( default : 4273 )

