#pragma once

#include "IDetailCustomization.h"
#include "Containers/Ticker.h"

class USwui;

class FSwuiDetails : public IDetailCustomization
{
public:
	virtual ~FSwuiDetails();
	static TSharedRef<IDetailCustomization> MakeInstance();
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

	void ScheduleDebouncedRefresh();

private:
	void DoRefresh();

	TWeakObjectPtr<USwui> SwuiPtr;
	IDetailLayoutBuilder* CachedDetailBuilder = nullptr;
	FTSTicker::FDelegateHandle RefreshTickerHandle;
	bool bRefreshScheduled = false;
};
