#pragma once

#include "IDetailCustomization.h"

class USwuiNavigation;

class FSwuiNavigationDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TWeakObjectPtr<USwuiNavigation> NavPtr;
	IDetailLayoutBuilder* CachedDetailBuilder = nullptr;
};
