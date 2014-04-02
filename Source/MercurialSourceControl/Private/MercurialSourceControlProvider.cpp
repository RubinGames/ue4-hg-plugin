//-------------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2014 Vadim Macagon
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//-------------------------------------------------------------------------------

#include "MercurialSourceControlPrivatePCH.h"
#include "MercurialSourceControlProvider.h"
#include "MercurialSourceControlSettingsWidget.h"
#include "MercurialSourceControlCommand.h"
#include "MercurialSourceControlFileState.h"
#include "MessageLog.h"
#include "ScopedSourceControlProgress.h"

namespace MercurialSourceControl {

// for LOCTEXT()
#define LOCTEXT_NAMESPACE "MercurialSourceControl"

static const char* SourceControl = "SourceControl";
static FName ProviderName("Mercurial");

void FProvider::Init(bool bForceConnection)
{
	// load setting from the command line or an INI file
	// we currently don't have any settings
}

void FProvider::Close()
{
	// clear out the file state cache
	FileStateMap.Empty();
}

const FName& FProvider::GetName() const
{
	return ProviderName;
}

FString FProvider::GetStatusText() const
{
	FString Text = LOCTEXT("ProviderName", "Provider: Mercurial").ToString();
	Text += LINE_TERMINATOR;
	FFormatNamedArguments Arguments;
	Arguments.Add(TEXT("YesOrNo"), IsEnabled() ? LOCTEXT("Yes", "Yes") : LOCTEXT("No", "No"));
	Text += FText::Format(LOCTEXT("EnabledLabel", "Enabled: {YesOrNo}"), Arguments).ToString();
	return Text;
}

bool FProvider::IsEnabled() const
{
	return true;
}

bool FProvider::IsAvailable() const
{
	return true;
}

ECommandResult::Type FProvider::GetState(
	const TArray<FString>& InFiles, 
	TArray< TSharedRef<ISourceControlState, ESPMode::ThreadSafe> >& OutState, 
	EStateCacheUsage::Type InStateCacheUsage
)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	// TODO: update the cache if requested to do so

	// retrieve the states for the given files from the cache
	for (auto Iterator(InFiles.CreateConstIterator()); Iterator; Iterator++)
	{
		auto* FileState = FileStateMap.Find(*Iterator);
		if (FileState)
		{
			OutState.Add(*FileState);
		}
		else
		{
			FFileStateRef DefaultFileState = MakeShareable(new FFileState(*Iterator));
			FileStateMap.Add(*Iterator, DefaultFileState);
			OutState.Add(DefaultFileState);
		}
	}

	return ECommandResult::Succeeded;
}

void FProvider::RegisterSourceControlStateChanged(
	const FSourceControlStateChanged::FDelegate& SourceControlStateChanged
)
{
	OnSourceControlStateChanged.Add(SourceControlStateChanged);
}

void FProvider::UnregisterSourceControlStateChanged(
	const FSourceControlStateChanged::FDelegate& SourceControlStateChanged
)
{
	OnSourceControlStateChanged.Remove(SourceControlStateChanged);
}

ECommandResult::Type FProvider::Execute(
	const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation,
	const TArray<FString>& InFiles, 
	EConcurrency::Type InConcurrency,
	const FSourceControlOperationComplete& InOperationCompleteDelegate
)
{
	if (!IsEnabled())
	{
		return ECommandResult::Failed;
	}

	// attempt to create a worker to perform the requested operation
	FWorkerPtr WorkerPtr = CreateWorker(InOperation->GetName());
	if (!WorkerPtr.IsValid())
	{
		// apparently we don't support this particular operation
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("OperationName"), FText::FromName(InOperation->GetName()));
		Arguments.Add(TEXT("ProviderName"), FText::FromName(GetName()));
		FMessageLog(SourceControl).Error(
			FText::Format(
				LOCTEXT(
					"UnsupportedOperation", 
					"Operation '{OperationName}' not supported by source control provider '{ProviderName}'"
				),
				Arguments
			)
		);
		return ECommandResult::Failed;
	}
	
	auto* Command = new FCommand(
		InOperation, WorkerPtr.ToSharedRef(), InOperationCompleteDelegate
	);

	if (InConcurrency == EConcurrency::Synchronous)
	{
		auto Result = ExecuteSynchronousCommand(Command, InOperation->GetInProgressString());
		delete Command;
		return Result;
	}
	else
	{
		return ExecuteCommand(Command, true);
	}
}

bool FProvider::CanCancelOperation(
	const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation
) const
{
	// don't support cancellation
	return false;
}

void FProvider::CancelOperation(
	const TSharedRef<ISourceControlOperation, ESPMode::ThreadSafe>& InOperation
)
{
	// nothing to do here
}

TArray< TSharedRef<class ISourceControlLabel> > FProvider::GetLabels(
	const FString& InMatchingSpec
) const
{
	TArray< TSharedRef<class ISourceControlLabel> > Labels;
	// TODO: figure out if this needs to be implemented
	return Labels;
}

bool FProvider::UsesLocalReadOnlyState() const
{
	return false;
}

void FProvider::Tick()
{
	bool bNotifyStateChanged = false;

	// remove commands that have finished executing from the queue
	for (int32 i = 0; i < CommandQueue.Num(); ++i)
	{
		FCommandQueueEntry CommandQueueEntry = CommandQueue[i];
		auto* Command = CommandQueueEntry.Command;
		if (Command->HasExecuted())
		{
			CommandQueue.RemoveAt(i);
			bNotifyStateChanged = Command->UpdateStates();
			LogCommandMessages(*Command);
			Command->NotifyOperationComplete();

			if (CommandQueueEntry.bAutoDelete)
			{
				delete Command;
			}

			// NotifyOperationComplete() may indirectly alter the command queue 
			// so the loop must stop here, the queue cleanup will resume on the
			// next tick.
			break;
		}
	}

	if (bNotifyStateChanged)
	{
		OnSourceControlStateChanged.Broadcast();
	}
}

TSharedRef<class SWidget> FProvider::MakeSettingsWidget() const
{
	return SNew(SSettingsWidget);
}

void FProvider::RegisterWorkerCreator(
	const FName& InOperationName, 
	const FCreateWorker& InDelegate
)
{
	WorkerCreatorsMap.Add(InOperationName, InDelegate);
}

ECommandResult::Type FProvider::ExecuteSynchronousCommand(
	FCommand* Command, const FText& ProgressText
)
{
	auto Result = ECommandResult::Failed;

	// display a progress dialog if progress text was provided
	{
		FScopedSourceControlProgress Progress(ProgressText);

		// attempt to execute the command asynchronously
		ExecuteCommand(Command, false);

		// wait for the command to finish executing
		while (!Command->HasExecuted())
		{
			Tick();
			Progress.Tick();
			FPlatformProcess::Sleep(0.01f);
		}

		// make sure the command queue is cleaned up
		Tick();

		if (Command->bCommandSuccessful)
		{
			Result = ECommandResult::Succeeded;
		}
	}

	return Result;
}

ECommandResult::Type FProvider::ExecuteCommand(FCommand* Command, bool bAutoDelete)
{
	if (GThreadPool)
	{
		GThreadPool->AddQueuedWork(Command);
		CommandQueue.Add({Command, bAutoDelete});
		return ECommandResult::Succeeded;
	}
	else // fall back to synchronous execution
	{
		Command->DoWork();
		Command->UpdateStates();
		LogCommandMessages(*Command);
		Command->NotifyOperationComplete();
		
		auto Result = Command->GetResult();
		if (bAutoDelete)
		{
			delete Command;
		}
		return Result;
	}
}

void FProvider::LogCommandMessages(const FCommand& InCommand)
{
	// TODO
}

FWorkerPtr FProvider::CreateWorker(const FName& InOperationName) const
{
	const auto* CreateWorkerPtr = WorkerCreatorsMap.Find(InOperationName);
	if (CreateWorkerPtr)
	{
		return (*CreateWorkerPtr)();
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE

} // namespace namespace MercurialSourceControl