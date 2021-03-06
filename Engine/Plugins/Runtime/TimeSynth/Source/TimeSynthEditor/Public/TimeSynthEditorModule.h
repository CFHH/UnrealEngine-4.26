// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "AudioEditorModule.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"


class FTimeSynthEditorModule : public IModuleInterface
{
public:

	// IModuleInterface interface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:

	void RegisterMenus();
};
