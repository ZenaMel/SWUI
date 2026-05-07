#pragma once

#include "IDetailCustomization.h"

class USwui;

class FSwuiDetails : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	TWeakObjectPtr<USwui> SwuiPtr;
	IDetailLayoutBuilder* CachedDetailBuilder = nullptr;
};
