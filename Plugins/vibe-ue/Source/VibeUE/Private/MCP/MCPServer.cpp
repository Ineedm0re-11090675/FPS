// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "MCP/MCPServer.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Core/ToolRegistry.h"
#include "Chat/MCPTypes.h"
#include "Async/Async.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Tools/ToolUsageTelemetry.h"

DEFINE_LOG_CATEGORY(LogMCPServer);

TSharedPtr<FMCPServer> FMCPServer::Instance;

// MCP Protocol versions we support (newest first)
static const TArray<FString> SUPPORTED_PROTOCOL_VERSIONS = {
    TEXT("2025-11-25"),  // Latest spec
    TEXT("2025-06-18"),  // Mid-2025 spec
    TEXT("2024-11-05")   // For compatibility with older clients
};
static const FString MCP_SERVER_NAME = TEXT("VibeUE");
static const FString MCP_SERVER_VERSION = TEXT("1.0.0");

namespace
{
    constexpr int32 kDefaultPort = 8101;
    constexpr int32 kMinPort = 1024;
    constexpr int32 kMaxPort = 65535;

    constexpr int32 kDefaultRequestReadTimeoutMs = 15000;
    constexpr int32 kMinRequestReadTimeoutMs = 1000;
    constexpr int32 kMaxRequestReadTimeoutMs = 300000;

    constexpr int32 kDefaultToolExecutionTimeoutMs = 60000;
    constexpr int32 kMinToolExecutionTimeoutMs = 1000;
    constexpr int32 kMaxToolExecutionTimeoutMs = 3600000;

    struct FMCPToolImpactSnapshot
    {
        TSet<FString> DirtyPackages;
        int32 ActorCount = 0;
    };

    struct FMCPStructuredToolResult
    {
        bool bParsedJson = false;
        bool bHasSuccessField = false;
        bool bSuccess = false;

        FString ActionName;
        FString Summary;
        FString ErrorCode;
        FString ErrorMessage;

        TArray<FString> ChangedItems;
        TArray<FString> VerificationHints;
        TArray<FString> Warnings;
        TArray<FString> AffectedAssets;
    };

    static TArray<FString> JsonArrayToStringArray(const TSharedPtr<FJsonObject>& JsonObject, const TCHAR* FieldName)
    {
        TArray<FString> Values;
        if (!JsonObject.IsValid())
        {
            return Values;
        }

        const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
        if (!JsonObject->TryGetArrayField(FieldName, JsonArray) || !JsonArray)
        {
            return Values;
        }

        Values.Reserve(JsonArray->Num());
        for (const TSharedPtr<FJsonValue>& Entry : *JsonArray)
        {
            FString StringValue;
            if (Entry.IsValid() && Entry->TryGetString(StringValue) && !StringValue.IsEmpty())
            {
                Values.Add(StringValue);
            }
        }

        return Values;
    }

    static FString ExtractToolActionName(const TMap<FString, FString>& Arguments)
    {
        if (const FString* ActionValue = Arguments.Find(TEXT("Action")))
        {
            return *ActionValue;
        }
        if (const FString* ActionValue = Arguments.Find(TEXT("action")))
        {
            return *ActionValue;
        }

        const FString* ParamsJson = Arguments.Find(TEXT("ParamsJson"));
        if (!ParamsJson)
        {
            return FString();
        }

        TSharedPtr<FJsonObject> ParamsObject;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ParamsJson);
        if (!FJsonSerializer::Deserialize(Reader, ParamsObject) || !ParamsObject.IsValid())
        {
            return FString();
        }

        FString ActionName;
        if (ParamsObject->TryGetStringField(TEXT("action"), ActionName))
        {
            return ActionName;
        }
        if (ParamsObject->TryGetStringField(TEXT("Action"), ActionName))
        {
            return ActionName;
        }

        return FString();
    }

    static FMCPStructuredToolResult ParseStructuredToolResult(const FString& ToolResult)
    {
        FMCPStructuredToolResult Parsed;

        TSharedPtr<FJsonObject> ResultJson;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ToolResult);
        if (!FJsonSerializer::Deserialize(Reader, ResultJson) || !ResultJson.IsValid())
        {
            return Parsed;
        }

        Parsed.bParsedJson = true;
        Parsed.bHasSuccessField = ResultJson->HasField(TEXT("success"));
        if (Parsed.bHasSuccessField)
        {
            ResultJson->TryGetBoolField(TEXT("success"), Parsed.bSuccess);
        }

        ResultJson->TryGetStringField(TEXT("action"), Parsed.ActionName);
        ResultJson->TryGetStringField(TEXT("summary"), Parsed.Summary);
        ResultJson->TryGetStringField(TEXT("error_code"), Parsed.ErrorCode);

        if (!ResultJson->TryGetStringField(TEXT("error"), Parsed.ErrorMessage) || Parsed.ErrorMessage.IsEmpty())
        {
            ResultJson->TryGetStringField(TEXT("error_message"), Parsed.ErrorMessage);
        }

        Parsed.ChangedItems = JsonArrayToStringArray(ResultJson, TEXT("changed_items"));
        Parsed.VerificationHints = JsonArrayToStringArray(ResultJson, TEXT("verification_hints"));
        Parsed.Warnings = JsonArrayToStringArray(ResultJson, TEXT("warnings"));
        Parsed.AffectedAssets = JsonArrayToStringArray(ResultJson, TEXT("affected_assets"));

        return Parsed;
    }

    static FMCPToolImpactSnapshot CaptureToolImpactSnapshot()
    {
        FMCPToolImpactSnapshot Snapshot;

#if WITH_EDITOR
        TArray<UPackage*> DirtyContentPackages;
        FEditorFileUtils::GetDirtyContentPackages(DirtyContentPackages);
        TArray<UPackage*> DirtyWorldPackages;
        FEditorFileUtils::GetDirtyWorldPackages(DirtyWorldPackages);

        auto AddPackages = [&Snapshot](const TArray<UPackage*>& Packages)
        {
            for (UPackage* Package : Packages)
            {
                if (!Package)
                {
                    continue;
                }
                Snapshot.DirtyPackages.Add(Package->GetName());
            }
        };
        AddPackages(DirtyContentPackages);
        AddPackages(DirtyWorldPackages);

        if (GEditor)
        {
            if (UWorld* World = GEditor->GetEditorWorldContext().World())
            {
                int32 Count = 0;
                for (TActorIterator<AActor> It(World); It; ++It)
                {
                    ++Count;
                }
                Snapshot.ActorCount = Count;
            }
        }
#endif

        return Snapshot;
    }

    static void ApplyImpactDelta(const FMCPToolImpactSnapshot& Before, const FMCPToolImpactSnapshot& After, FMCPToolCallRecord& InOutRecord)
    {
        InOutRecord.ActorCountDelta = After.ActorCount - Before.ActorCount;

        for (const FString& PackageName : After.DirtyPackages)
        {
            if (!Before.DirtyPackages.Contains(PackageName))
            {
                InOutRecord.NewDirtyPackages.Add(PackageName);
            }
        }

        InOutRecord.NewDirtyPackages.Sort();
        const int32 TotalNewDirtyPackages = InOutRecord.NewDirtyPackages.Num();
        InOutRecord.NewDirtyPackageCount = TotalNewDirtyPackages;

        constexpr int32 MaxPackagesToKeep = 20;
        if (TotalNewDirtyPackages > MaxPackagesToKeep)
        {
            InOutRecord.NewDirtyPackages.SetNum(MaxPackagesToKeep);
        }
    }

    static int32 ClampConfiguredValue(const int32 Value, const int32 DefaultValue, const int32 MinValue, const int32 MaxValue)
    {
        if (Value <= 0)
        {
            return DefaultValue;
        }
        return FMath::Clamp(Value, MinValue, MaxValue);
    }

    static bool TryGetCommandLineIntOverride(const TCHAR* ParamName, int32& OutValue)
    {
        const TCHAR* CommandLine = FCommandLine::Get();
        if (!CommandLine || !ParamName)
        {
            return false;
        }

        int32 ParsedValue = 0;
        if (!FParse::Value(CommandLine, ParamName, ParsedValue))
        {
            return false;
        }

        OutValue = ParsedValue;
        return true;
    }
}

FMCPServer::FMCPServer()
    : NextEventId(0)
{
}

FMCPServer::~FMCPServer()
{
    Shutdown();
}

FMCPServer& FMCPServer::Get()
{
    if (!Instance.IsValid())
    {
        Instance = MakeShared<FMCPServer>();
    }
    return *Instance;
}

void FMCPServer::Initialize()
{
    LoadConfig();
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server initialized - Enabled: %s, Port: %d, RequestReadTimeoutMs: %d, ToolExecutionTimeoutMs: %d, Bearer Token: %s"),
        Config.bEnabled ? TEXT("Yes") : TEXT("No"),
        Config.Port,
        Config.RequestReadTimeoutMs,
        Config.ToolExecutionTimeoutMs,
        Config.ApiKey.IsEmpty() ? TEXT("(none)") : TEXT("(set)"));
    
    // Auto-start if enabled
    if (Config.bEnabled)
    {
        Start();
    }
}

void FMCPServer::Shutdown()
{
    StopServer();
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server shutdown"));
    
    // Reset the static instance to ensure proper cleanup on editor exit
    // This prevents the server from keeping the module alive
    Instance.Reset();
}

bool FMCPServer::Start()
{
    if (bIsRunning)
    {
        UE_LOG(LogMCPServer, Warning, TEXT("MCP Server already running"));
        return true;
    }

    UE_LOG(LogMCPServer, Log, TEXT("Starting MCP Server on port %d..."), Config.Port);
    RecordDiagnosticsEvent(TEXT("server start"));
    
    // Create TCP listener socket
    ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!SocketSubsystem)
    {
        UE_LOG(LogMCPServer, Error, TEXT("Failed to get socket subsystem"));
        return false;
    }
    
    // Bind to localhost only for security (prevent DNS rebinding attacks)
    FIPv4Address LocalAddress;
    FIPv4Address::Parse(TEXT("127.0.0.1"), LocalAddress);
    
    TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
    Addr->SetIp(LocalAddress.Value);
    Addr->SetPort(Config.Port);
    
    ListenerSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MCP Server"), false);
    if (!ListenerSocket)
    {
        UE_LOG(LogMCPServer, Error, TEXT("Failed to create listener socket"));
        return false;
    }
    
    // Set socket options
    ListenerSocket->SetReuseAddr(true);
    ListenerSocket->SetNonBlocking(true);
    
    // Bind and listen
    if (!ListenerSocket->Bind(*Addr))
    {
        UE_LOG(LogMCPServer, Error, TEXT("Failed to bind to port %d - is another process using it?"), Config.Port);
        SocketSubsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        return false;
    }
    
    if (!ListenerSocket->Listen(8))
    {
        UE_LOG(LogMCPServer, Error, TEXT("Failed to listen on socket"));
        SocketSubsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        return false;
    }
    
    bIsRunning = true;
    bShouldStop = false;
    
    // Start server thread
    ServerThread = FRunnableThread::Create(this, TEXT("MCPServerThread"), 0, TPri_Normal);
    if (!ServerThread)
    {
        UE_LOG(LogMCPServer, Error, TEXT("Failed to create server thread"));
        SocketSubsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        bIsRunning = false;
        return false;
    }
    
    // Register tick delegate for processing requests on game thread
    TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([this](float DeltaTime) -> bool
        {
            ProcessPendingRequests();
            return true; // Keep ticking
        }),
        0.016f // ~60 Hz
    );
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server started at %s"), *GetServerUrl());
    
    return true;
}

void FMCPServer::StopServer()
{
    if (!bIsRunning)
    {
        return;
    }
    
    UE_LOG(LogMCPServer, Log, TEXT("Stopping MCP Server..."));
    RecordDiagnosticsEvent(TEXT("server stop"));
    
    bShouldStop = true;
    bIsRunning = false;
    
    // Remove tick delegate
    if (TickDelegateHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
        TickDelegateHandle.Reset();
    }
    
    // Close listener socket FIRST to unblock the WaitForPendingConnection call in the thread
    if (ListenerSocket)
    {
        ListenerSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
    }
    
    // Now wait for thread to finish (it should exit quickly since socket is closed)
    if (ServerThread)
    {
        ServerThread->WaitForCompletion();
        delete ServerThread;
        ServerThread = nullptr;
    }
    
    // Clear sessions
    FScopeLock Lock(&SessionLock);
    ActiveSessions.Empty();
    
    // Clean up SSE connections
    {
        FScopeLock SSELockGuard(&SSELock);
        for (auto& Connection : SSEConnections)
        {
            if (Connection->ClientSocket)
            {
                Connection->ClientSocket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Connection->ClientSocket);
            }
        }
        SSEConnections.Empty();
    }
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server stopped"));
}

FString FMCPServer::GetServerUrl() const
{
    return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Config.Port);
}

// ============ FRunnable Interface ============

bool FMCPServer::Init()
{
    return true;
}

uint32 FMCPServer::Run()
{
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server thread started"));
    
    while (!bShouldStop)
    {
        // Check if socket is still valid (may have been closed during shutdown)
        if (!ListenerSocket)
        {
            break;
        }
        
        bool bHasPendingConnection = false;
        if (ListenerSocket->WaitForPendingConnection(bHasPendingConnection, FTimespan::FromMilliseconds(100)))
        {
            if (bHasPendingConnection && !bShouldStop)
            {
                FSocket* ClientSocket = ListenerSocket->Accept(TEXT("MCP Client"));
                if (ClientSocket)
                {
                    UE_LOG(LogMCPServer, Log, TEXT("MCP: New connection accepted"));
                    {
                        FScopeLock Lock(&DiagnosticsLock);
                        Diagnostics.TotalConnectionsAccepted++;
                        Diagnostics.LastConnectionAcceptedUtc = FDateTime::UtcNow();
                    }
                    RecordDiagnosticsEvent(TEXT("conn accepted"));
                    HandleConnection(ClientSocket);
                    UE_LOG(LogMCPServer, Log, TEXT("MCP: Connection handling completed"));
                }
            }
        }
        
        // Small sleep to prevent busy-waiting
        FPlatformProcess::Sleep(0.001f);
    }
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP Server thread exiting"));
    return 0;
}

void FMCPServer::Exit()
{
    // Nothing to cleanup here - Stop() handles it
}

// ============ HTTP Handling ============

bool FMCPServer::HandleConnection(FSocket* ClientSocket)
{
    if (!ClientSocket)
    {
        return false;
    }
    
    FString Method, Path;
    TMap<FString, FString> Headers;
    FString Body;
    
    if (!ParseHttpRequest(ClientSocket, Method, Path, Headers, Body))
    {
        RecordDiagnosticsEvent(TEXT("http parse failed"));
        SendHttpResponse(ClientSocket, 400, TEXT("Bad Request"), TEXT("text/plain"), TEXT("Invalid HTTP request"));
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        return false;
    }
    
    UE_LOG(LogMCPServer, Verbose, TEXT("MCP Request: %s %s"), *Method, *Path);
    RecordDiagnosticsEvent(FString::Printf(TEXT("http %s %s"), *Method, *Path));
    
    // Only handle /mcp endpoint
    if (!Path.StartsWith(TEXT("/mcp")))
    {
        SendHttpResponse(ClientSocket, 404, TEXT("Not Found"), TEXT("text/plain"), TEXT("Not Found"));
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        return false;
    }
    
    // Validate Origin header for security
    if (!ValidateOrigin(Headers))
    {
        SendHttpResponse(ClientSocket, 403, TEXT("Forbidden"), TEXT("text/plain"), TEXT("Invalid Origin"));
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        return false;
    }
    
    // Validate API key if configured
    if (!ValidateApiKey(Headers))
    {
        SendHttpResponse(ClientSocket, 401, TEXT("Unauthorized"), TEXT("text/plain"), TEXT("Invalid or missing API key"));
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        return false;
    }
    
    // Validate MCP-Protocol-Version header (for non-initialize requests)
    FString SessionId = Headers.FindRef(TEXT("mcp-session-id"));
    if (!SessionId.IsEmpty() && !ValidateProtocolVersion(Headers))
    {
        SendHttpResponse(ClientSocket, 400, TEXT("Bad Request"), TEXT("text/plain"), TEXT("Invalid or unsupported MCP-Protocol-Version"));
        ClientSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
        return false;
    }
    
    // Handle different HTTP methods per MCP spec
    if (Method == TEXT("POST"))
    {
        UE_LOG(LogMCPServer, Log, TEXT("MCP POST: Processing JSON-RPC request"));
        
        // Process JSON-RPC request (SessionId already retrieved above)
        bool bIsNotification = false;
        FString Response = HandleMCPRequest(Body, SessionId, bIsNotification);
        
        UE_LOG(LogMCPServer, Log, TEXT("MCP POST: Request processed, notification=%d, response length=%d"), 
            bIsNotification, Response.Len());
        
        if (bIsNotification)
        {
            // Notifications return 202 Accepted with no body
            SendHttpResponse(ClientSocket, 202, TEXT("Accepted"), TEXT(""), TEXT(""));
        }
        else
        {
            // Build response headers
            TMap<FString, FString> ResponseHeaders;
            
            // If this was an initialize request and we generated a session ID, include it
            if (!SessionId.IsEmpty())
            {
                ResponseHeaders.Add(TEXT("Mcp-Session-Id"), SessionId);
            }
            
            // Add CORS headers
            ResponseHeaders.Add(TEXT("Access-Control-Allow-Origin"), TEXT("*"));
            ResponseHeaders.Add(TEXT("Access-Control-Allow-Headers"), TEXT("Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Accept"));
            
            // Check if client wants SSE response
            if (AcceptsSSE(Headers))
            {
                // Send as SSE stream (single event then close for POST)
                SendSSEResponse(ClientSocket, SessionId);
                int32 EventId = ++NextEventId;
                SendSSEEvent(ClientSocket, Response, EventId);
                // Note: Don't close socket here - let client close or we send more events
                // For POST, we close after sending the response
                ClientSocket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
                return true;
            }
            else
            {
                UE_LOG(LogMCPServer, Log, TEXT("MCP POST: Sending HTTP response"));
                SendHttpResponse(ClientSocket, 200, TEXT("OK"), TEXT("application/json"), Response, ResponseHeaders);
                UE_LOG(LogMCPServer, Log, TEXT("MCP POST: Response sent"));
            }
        }
    }
    else if (Method == TEXT("GET"))
    {
        // GET opens SSE stream for server-to-client messages
        if (!AcceptsSSE(Headers))
        {
            SendHttpResponse(ClientSocket, 406, TEXT("Not Acceptable"), TEXT("text/plain"), 
                TEXT("GET requests must accept text/event-stream"));
            ClientSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
            return false;
        }
        
        // Handle SSE stream request
        return HandleSSERequest(ClientSocket, Headers);
    }
    else if (Method == TEXT("DELETE"))
    {
        // Session termination (SessionId already retrieved above)
        if (!SessionId.IsEmpty())
        {
            FScopeLock Lock(&SessionLock);
            ActiveSessions.Remove(SessionId);
            UE_LOG(LogMCPServer, Log, TEXT("Session terminated: %s"), *SessionId);
        }
        SendHttpResponse(ClientSocket, 200, TEXT("OK"), TEXT("text/plain"), TEXT("Session terminated"));
    }
    else if (Method == TEXT("OPTIONS"))
    {
        // CORS preflight
        TMap<FString, FString> CorsHeaders;
        CorsHeaders.Add(TEXT("Access-Control-Allow-Origin"), TEXT("*"));
        CorsHeaders.Add(TEXT("Access-Control-Allow-Methods"), TEXT("GET, POST, DELETE, OPTIONS"));
        CorsHeaders.Add(TEXT("Access-Control-Allow-Headers"), TEXT("Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Accept"));
        CorsHeaders.Add(TEXT("Access-Control-Max-Age"), TEXT("86400"));
        SendHttpResponse(ClientSocket, 204, TEXT("No Content"), TEXT(""), TEXT(""), CorsHeaders);
    }
    else
    {
        SendHttpResponse(ClientSocket, 405, TEXT("Method Not Allowed"), TEXT("text/plain"), TEXT("Method not allowed"));
    }
    
    ClientSocket->Close();
    ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
    return true;
}

bool FMCPServer::ParseHttpRequest(FSocket* Socket, FString& OutMethod, FString& OutPath,
                                   TMap<FString, FString>& OutHeaders, FString& OutBody)
{
    if (!Socket)
    {
        return false;
    }
    
    // Read request with timeout
    TArray<uint8> Buffer;
    Buffer.SetNumUninitialized(8192);
    
    int32 BytesRead = 0;
    FString RequestData;
    
    // Wait for data with timeout
    Socket->SetNonBlocking(false);
    Socket->SetReceiveBufferSize(8192, BytesRead);
    
    // Read in chunks until we have the full request
    int32 TotalRead = 0;
    const int32 MaxSize = 1024 * 1024; // 1MB max request
    int32 EmptyReadCount = 0;
    const int32 MaxEmptyReads = 3;
    const int32 RequestReadTimeoutMs = FMath::Max(Config.RequestReadTimeoutMs, kMinRequestReadTimeoutMs);
    const double WaitSliceSeconds = FMath::Max(
        static_cast<double>(RequestReadTimeoutMs) / static_cast<double>(MaxEmptyReads) / 1000.0,
        0.1);
    
    while (TotalRead < MaxSize && EmptyReadCount < MaxEmptyReads)
    {
        bool bHadData = Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(WaitSliceSeconds));
        
        if (!bHadData)
        {
            EmptyReadCount++;
            UE_LOG(LogMCPServer, Warning, TEXT("ParseHttpRequest: No data after wait (attempt %d/%d)"), EmptyReadCount, MaxEmptyReads);
            continue;
        }
        
        BytesRead = 0;
        if (Socket->Recv(Buffer.GetData(), Buffer.Num() - 1, BytesRead))
        {
            if (BytesRead > 0)
            {
                EmptyReadCount = 0; // Reset counter on successful read
                Buffer[BytesRead] = 0;
                RequestData += UTF8_TO_TCHAR(reinterpret_cast<const char*>(Buffer.GetData()));
                TotalRead += BytesRead;
                
                // Check if we have complete headers
                int32 HeaderEnd = RequestData.Find(TEXT("\r\n\r\n"));
                if (HeaderEnd != INDEX_NONE)
                {
                    // Parse Content-Length to know if we need more body
                    FString HeaderPart = RequestData.Left(HeaderEnd);
                    int32 ContentLength = 0;
                    
                    TArray<FString> HeaderLines;
                    HeaderPart.ParseIntoArray(HeaderLines, TEXT("\r\n"));
                    
                    for (const FString& Line : HeaderLines)
                    {
                        if (Line.StartsWith(TEXT("Content-Length:"), ESearchCase::IgnoreCase))
                        {
                            FString LengthStr = Line.Mid(15).TrimStartAndEnd();
                            ContentLength = FCString::Atoi(*LengthStr);
                            break;
                        }
                    }
                    
                    // Check if we have the full body
                    int32 BodyStart = HeaderEnd + 4;
                    int32 BodyLength = RequestData.Len() - BodyStart;
                    
                    if (BodyLength >= ContentLength)
                    {
                        break; // We have everything
                    }
                }
            }
            else
            {
                // Connection closed or no more data
                UE_LOG(LogMCPServer, Log, TEXT("ParseHttpRequest: Connection closed (0 bytes read)"));
                break;
            }
        }
        else
        {
            UE_LOG(LogMCPServer, Warning, TEXT("ParseHttpRequest: Recv() failed"));
            break;
        }
    }
    
    if (EmptyReadCount >= MaxEmptyReads)
    {
        UE_LOG(LogMCPServer, Warning, TEXT("ParseHttpRequest: Timeout waiting for data after %d ms"), RequestReadTimeoutMs);
    }
    
    if (RequestData.IsEmpty())
    {
        UE_LOG(LogMCPServer, Warning, TEXT("ParseHttpRequest: No request data received"));
        return false;
    }
    
    // Parse request line
    int32 FirstLineEnd = RequestData.Find(TEXT("\r\n"));
    if (FirstLineEnd == INDEX_NONE)
    {
        return false;
    }
    
    FString RequestLine = RequestData.Left(FirstLineEnd);
    TArray<FString> RequestParts;
    RequestLine.ParseIntoArrayWS(RequestParts);
    
    if (RequestParts.Num() < 2)
    {
        return false;
    }
    
    OutMethod = RequestParts[0];
    OutPath = RequestParts[1];
    
    // Parse headers
    int32 HeaderEnd = RequestData.Find(TEXT("\r\n\r\n"));
    if (HeaderEnd == INDEX_NONE)
    {
        return false;
    }
    
    FString HeaderSection = RequestData.Mid(FirstLineEnd + 2, HeaderEnd - FirstLineEnd - 2);
    TArray<FString> HeaderLines;
    HeaderSection.ParseIntoArray(HeaderLines, TEXT("\r\n"));
    
    for (const FString& Line : HeaderLines)
    {
        int32 ColonPos;
        if (Line.FindChar(':', ColonPos))
        {
            FString Key = Line.Left(ColonPos).TrimStartAndEnd().ToLower();
            FString Value = Line.Mid(ColonPos + 1).TrimStartAndEnd();
            OutHeaders.Add(Key, Value);
        }
    }
    
    // Extract body
    OutBody = RequestData.Mid(HeaderEnd + 4);
    
    return true;
}

void FMCPServer::SendHttpResponse(FSocket* Socket, int32 StatusCode, const FString& StatusText,
                                   const FString& ContentType, const FString& Body,
                                   const TMap<FString, FString>& ExtraHeaders)
{
    if (!Socket)
    {
        UE_LOG(LogMCPServer, Warning, TEXT("SendHttpResponse called with null socket"));
        return;
    }
    
    // Convert body to UTF-8 FIRST to get accurate byte length for Content-Length header
    // Using Body.Len() is incorrect because UTF-8 can have more bytes than characters
    FTCHARToUTF8 BodyConverter(*Body);
    int32 BodyByteLength = BodyConverter.Length();
    
    FString Response = FString::Printf(TEXT("HTTP/1.1 %d %s\r\n"), StatusCode, *StatusText);
    
    if (!ContentType.IsEmpty())
    {
        Response += FString::Printf(TEXT("Content-Type: %s; charset=utf-8\r\n"), *ContentType);
    }
    
    Response += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyByteLength);
    Response += TEXT("Connection: close\r\n");
    
    // Add extra headers
    for (const auto& Header : ExtraHeaders)
    {
        Response += FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
    }
    
    Response += TEXT("\r\n");
    
    // Convert headers to UTF-8 and send separately, then send body bytes
    FTCHARToUTF8 HeaderConverter(*Response);
    
    // Combine headers and body into one buffer for atomic send
    TArray<uint8> SendBuffer;
    SendBuffer.SetNumUninitialized(HeaderConverter.Length() + BodyByteLength);
    FMemory::Memcpy(SendBuffer.GetData(), HeaderConverter.Get(), HeaderConverter.Length());
    FMemory::Memcpy(SendBuffer.GetData() + HeaderConverter.Length(), BodyConverter.Get(), BodyByteLength);
    
    int32 BytesSent = 0;
    bool bSendSuccess = Socket->Send(SendBuffer.GetData(), SendBuffer.Num(), BytesSent);
    
    if (!bSendSuccess || BytesSent != SendBuffer.Num())
    {
        UE_LOG(LogMCPServer, Warning, TEXT("SendHttpResponse: Send incomplete/failed - sent %d of %d bytes"), 
            BytesSent, SendBuffer.Num());
    }
    else
    {
        UE_LOG(LogMCPServer, Verbose, TEXT("SendHttpResponse: %d %s - %d bytes (body: %d bytes)"), 
            StatusCode, *StatusText, BytesSent, BodyByteLength);
    }
}

// ============ MCP Protocol Handling ============

FString FMCPServer::HandleMCPRequest(const FString& JsonBody, FString& InOutSessionId, bool& bOutIsNotification)
{
    bOutIsNotification = false;
    
    TSharedPtr<FJsonObject> RequestObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonBody);
    
    if (!FJsonSerializer::Deserialize(Reader, RequestObj) || !RequestObj.IsValid())
    {
        return BuildJsonRpcError(TEXT(""), -32700, TEXT("Parse error"));
    }
    
    // Check JSON-RPC version
    FString JsonRpc;
    if (!RequestObj->TryGetStringField(TEXT("jsonrpc"), JsonRpc) || JsonRpc != TEXT("2.0"))
    {
        return BuildJsonRpcError(TEXT(""), -32600, TEXT("Invalid Request - missing or invalid jsonrpc version"));
    }
    
    // Get method
    FString Method;
    if (!RequestObj->TryGetStringField(TEXT("method"), Method))
    {
        return BuildJsonRpcError(TEXT(""), -32600, TEXT("Invalid Request - missing method"));
    }
    
    // Get request ID (if missing, it's a notification)
    FString RequestId;
    const TSharedPtr<FJsonValue>* IdValue = RequestObj->Values.Find(TEXT("id"));
    if (IdValue && (*IdValue)->Type != EJson::Null)
    {
        if ((*IdValue)->Type == EJson::String)
        {
            RequestId = (*IdValue)->AsString();
        }
        else if ((*IdValue)->Type == EJson::Number)
        {
            RequestId = FString::Printf(TEXT("%d"), (int32)(*IdValue)->AsNumber());
        }
    }
    else
    {
        bOutIsNotification = true;
    }
    
    // Get params
    TSharedPtr<FJsonObject> Params;
    const TSharedPtr<FJsonObject>* ParamsPtr;
    if (RequestObj->TryGetObjectField(TEXT("params"), ParamsPtr))
    {
        Params = *ParamsPtr;
    }
    
        UE_LOG(LogMCPServer, Log, TEXT("MCP Method: %s (id: %s)"), *Method, RequestId.IsEmpty() ? TEXT("<notification>") : *RequestId);
        {
            FScopeLock Lock(&DiagnosticsLock);
            Diagnostics.TotalRpcRequests++;
            Diagnostics.LastRpcUtc = FDateTime::UtcNow();
            Diagnostics.LastRpcMethod = Method;
            Diagnostics.LastRpcId = RequestId;
            Diagnostics.LastRpcSessionId = InOutSessionId;
        }
        RecordDiagnosticsEvent(FString::Printf(
            TEXT("rpc %s id=%s"),
            *Method,
            RequestId.IsEmpty() ? TEXT("<notification>") : *RequestId));
        
        // Route to handler
        if (Method == TEXT("initialize"))
        {
            FString Response = HandleInitialize(Params, RequestId);
            // Extract session ID from the initialized session for caller
            InOutSessionId = GenerateSessionId();
            {
                FScopeLock Lock(&SessionLock);
                ActiveSessions.Add(InOutSessionId, FDateTime::UtcNow());
            }
            {
                FScopeLock Lock(&DiagnosticsLock);
                Diagnostics.LastRpcSessionId = InOutSessionId;
            }
            RecordDiagnosticsEvent(FString::Printf(TEXT("session new %s"), *InOutSessionId));
            return Response;
        }
        else if (Method == TEXT("initialized") || Method == TEXT("notifications/initialized"))
        {
            // Client acknowledgment - nothing to return
            bOutIsNotification = true;
            return TEXT("");
        }
        else if (Method == TEXT("tools/list"))
        {
            return HandleToolsList(Params, RequestId);
        }
        else if (Method == TEXT("tools/call"))
        {
            return HandleToolsCall(Params, RequestId, InOutSessionId);
        }
        else if (Method == TEXT("resources/list"))
        {
            return HandleResourcesList(Params, RequestId);
        }
        else if (Method == TEXT("resources/templates/list"))
        {
            return HandleResourceTemplatesList(Params, RequestId);
        }
        else if (Method == TEXT("prompts/list"))
        {
            return HandlePromptsList(Params, RequestId);
        }
        else if (Method == TEXT("ping"))
        {
            return HandlePing(RequestId);
        }
    else if (Method == TEXT("notifications/cancelled"))
    {
        // Cancellation notification - just acknowledge
        bOutIsNotification = true;
        return TEXT("");
    }
    else
    {
        return BuildJsonRpcError(RequestId, -32601, FString::Printf(TEXT("Method not found: %s"), *Method));
    }
}

FString FMCPServer::HandleInitialize(TSharedPtr<FJsonObject> Params, const FString& RequestId)
{
    // Get client's requested protocol version
    FString RequestedVersion;
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("protocolVersion"), RequestedVersion);
    }
    
    // Negotiate protocol version
    FString NegotiatedVersion = SUPPORTED_PROTOCOL_VERSIONS[0]; // Default to latest
    
    if (!RequestedVersion.IsEmpty())
    {
        // Check if we support the client's requested version
        if (SUPPORTED_PROTOCOL_VERSIONS.Contains(RequestedVersion))
        {
            NegotiatedVersion = RequestedVersion;
            UE_LOG(LogMCPServer, Log, TEXT("MCP Initialize: Client requested %s, using exact match"), *RequestedVersion);
        }
        else
        {
            // Client requested unsupported version, use our latest
            UE_LOG(LogMCPServer, Warning, TEXT("MCP Initialize: Client requested unsupported version %s, using %s"), 
                   *RequestedVersion, *NegotiatedVersion);
        }
    }
    else
    {
        UE_LOG(LogMCPServer, Log, TEXT("MCP Initialize: No version requested, using latest %s"), *NegotiatedVersion);
    }
    
    // Build capabilities
    TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    
    // We support tools
    TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
    Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

    // Optional capability stubs for better client compatibility.
    // We currently expose no resources or prompts, but some clients call these methods unconditionally.
    Capabilities->SetObjectField(TEXT("resources"), MakeShared<FJsonObject>());
    Capabilities->SetObjectField(TEXT("prompts"), MakeShared<FJsonObject>());
    
    // Build server info
    TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
    ServerInfo->SetStringField(TEXT("name"), MCP_SERVER_NAME);
    ServerInfo->SetStringField(TEXT("version"), MCP_SERVER_VERSION);
    
    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("protocolVersion"), NegotiatedVersion);
    Result->SetObjectField(TEXT("capabilities"), Capabilities);
    Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
    
    // Build response - session ID is added as header by caller
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandleResourcesList(TSharedPtr<FJsonObject> /*Params*/, const FString& RequestId)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("resources"), TArray<TSharedPtr<FJsonValue>>());
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandleResourceTemplatesList(TSharedPtr<FJsonObject> /*Params*/, const FString& RequestId)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("resourceTemplates"), TArray<TSharedPtr<FJsonValue>>());
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandlePromptsList(TSharedPtr<FJsonObject> /*Params*/, const FString& RequestId)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("prompts"), TArray<TSharedPtr<FJsonValue>>());
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandleToolsList(TSharedPtr<FJsonObject> Params, const FString& RequestId)
{
    // Get internal tools
    TArray<FMCPTool> Tools = GetInternalTools();

    // Build tools array
    TArray<TSharedPtr<FJsonValue>> ToolsArray;
    
    for (const FMCPTool& Tool : Tools)
    {
        TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
        ToolObj->SetStringField(TEXT("name"), Tool.Name);
        ToolObj->SetStringField(TEXT("description"), Tool.Description);
        
        // Convert input schema
        if (Tool.InputSchema.IsValid())
        {
            ToolObj->SetObjectField(TEXT("inputSchema"), Tool.InputSchema);
        }
        else
        {
            // Default empty schema
            TSharedPtr<FJsonObject> EmptySchema = MakeShared<FJsonObject>();
            EmptySchema->SetStringField(TEXT("type"), TEXT("object"));
            EmptySchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
            ToolObj->SetObjectField(TEXT("inputSchema"), EmptySchema);
        }
        
        ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
    }
    
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("tools"), ToolsArray);
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP tools/list - Returning %d tools"), Tools.Num());
    
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandleToolsCall(TSharedPtr<FJsonObject> Params, const FString& RequestId, const FString& SessionId)
{
    if (!Params.IsValid())
    {
        return BuildJsonRpcError(RequestId, -32602, TEXT("Invalid params"));
    }
    
    FString ToolName;
    if (!Params->TryGetStringField(TEXT("name"), ToolName))
    {
        return BuildJsonRpcError(RequestId, -32602, TEXT("Missing tool name"));
    }

    const FDateTime ToolCallStartUtc = FDateTime::UtcNow();
    int64 ToolCallSequence = 0;
    {
        FScopeLock Lock(&DiagnosticsLock);
        Diagnostics.LastToolsCallUtc = ToolCallStartUtc;
        Diagnostics.LastToolsCallToolName = ToolName;
        Diagnostics.LastToolsCallActionName.Empty();
        Diagnostics.bLastToolsCallIsError = false;
        Diagnostics.LastToolsCallErrorSummary.Empty();

        ToolCallSequence = Diagnostics.NextToolCallSequence++;

        FMCPToolCallRecord Record;
        Record.Sequence = ToolCallSequence;
        Record.RequestId = RequestId;
        Record.SessionId = SessionId;
        Record.ToolName = ToolName;
        Record.StartedUtc = ToolCallStartUtc;
        Record.bInProgress = true;

        constexpr int32 MaxToolCalls = 50;
        const int32 Overflow = Diagnostics.RecentToolCalls.Num() - MaxToolCalls + 1;
        if (Overflow > 0)
        {
            Diagnostics.RecentToolCalls.RemoveAt(0, Overflow);
        }
        Diagnostics.RecentToolCalls.Add(MoveTemp(Record));
    }

    RecordDiagnosticsEvent(FString::Printf(TEXT("tools/call %s start"), *ToolName));
    double ToolStartTime = FPlatformTime::Seconds();

    auto CompleteToolRecord = [this, ToolCallSequence](const FMCPStructuredToolResult& StructuredResult, bool bIsError, const FString& ErrorSummary, double DurationMs, int32 ActorDelta, int32 NewDirtyPackageCount, const TArray<FString>& NewDirtyPackages)
    {
        FScopeLock Lock(&DiagnosticsLock);
        for (FMCPToolCallRecord& Record : Diagnostics.RecentToolCalls)
        {
            if (Record.Sequence != ToolCallSequence)
            {
                continue;
            }

            Record.bInProgress = false;
            Record.bIsError = bIsError;
            if (!StructuredResult.ActionName.IsEmpty())
            {
                Record.ActionName = StructuredResult.ActionName;
            }
            Record.Summary = StructuredResult.Summary.Left(300);
            Record.ErrorCode = StructuredResult.ErrorCode.Left(80);
            Record.ErrorSummary = bIsError ? ErrorSummary.Left(200) : FString();
            Record.ChangedItems = StructuredResult.ChangedItems;
            Record.VerificationHints = StructuredResult.VerificationHints;
            Record.Warnings = StructuredResult.Warnings;
            Record.AffectedAssets = StructuredResult.AffectedAssets;
            Record.CompletedUtc = FDateTime::UtcNow();
            Record.DurationMs = DurationMs;
            Record.ActorCountDelta = ActorDelta;
            Record.NewDirtyPackageCount = NewDirtyPackageCount;
            Record.NewDirtyPackages = NewDirtyPackages;
            break;
        }
    };

    {
        FScopeLock Lock(&DiagnosticsLock);
        Diagnostics.LastToolsCallUtc = ToolCallStartUtc;
        Diagnostics.LastToolsCallToolName = ToolName;
        Diagnostics.LastToolsCallActionName.Empty();
        Diagnostics.bLastToolsCallIsError = false;
        Diagnostics.LastToolsCallErrorSummary.Empty();
    }
    
    // Get arguments
    TMap<FString, FString> Arguments;
    const TSharedPtr<FJsonObject>* ArgsObj;
    if (Params->TryGetObjectField(TEXT("arguments"), ArgsObj))
    {
        for (const auto& Pair : (*ArgsObj)->Values)
        {
            if (Pair.Value->Type == EJson::String)
            {
                Arguments.Add(Pair.Key, Pair.Value->AsString());
            }
            else if (Pair.Value->Type == EJson::Number)
            {
                // Serialize numbers directly — FJsonSerializer::Serialize with an identifier
                // requires an open object context and produces empty output otherwise.
                Arguments.Add(Pair.Key, FString::Printf(TEXT("%.10g"), Pair.Value->AsNumber()));
            }
            else if (Pair.Value->Type == EJson::Boolean)
            {
                Arguments.Add(Pair.Key, Pair.Value->AsBool() ? TEXT("true") : TEXT("false"));
            }
            else
            {
                // Arrays/objects — serialize as condensed JSON
                FString JsonStr;
                TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                    TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
                FJsonSerializer::Serialize(Pair.Value->AsArray(), Writer);
                Arguments.Add(Pair.Key, JsonStr);
            }
        }
    }

    auto RecordToolUsage = [ToolName, RequestId, SessionId, Arguments](const FMCPStructuredToolResult& StructuredResult, bool bIsError, const FString& ErrorSummary, double DurationMs, int32 ActorDelta, int32 NewDirtyPackageCount, const TArray<FString>& NewDirtyPackages)
    {
        FToolUsageEvent Event;
        Event.TimestampUtc = FDateTime::UtcNow();
        Event.ToolName = ToolName;
        Event.ActionName = StructuredResult.ActionName;
        Event.RequestId = RequestId;
        Event.SessionId = SessionId;
        Event.bSuccess = !bIsError;
        Event.DurationMs = DurationMs;
        Event.ErrorCode = StructuredResult.ErrorCode;
        Event.ErrorSummary = bIsError ? ErrorSummary : FString();
        Event.ChangedItems = StructuredResult.ChangedItems;
        Event.VerificationHints = StructuredResult.VerificationHints;
        Event.Warnings = StructuredResult.Warnings;
        Event.AffectedAssets = StructuredResult.AffectedAssets;
        Event.ActorCountDelta = ActorDelta;
        Event.NewDirtyPackageCount = NewDirtyPackageCount;
        Event.NewDirtyPackages = NewDirtyPackages;
        Event.Arguments = Arguments;

        FString TelemetryError;
        FToolUsageTelemetry::RecordToolUsage(Event, TelemetryError);
    };
    
    UE_LOG(LogMCPServer, Log, TEXT("MCP tools/call - Tool: %s (RequestId: %s), Arguments received: %d"), *ToolName, *RequestId, Arguments.Num());
    for (const auto& Pair : Arguments)
    {
        UE_LOG(LogMCPServer, Log, TEXT("  Arg: %s = %s"), *Pair.Key, *Pair.Value);
    }
    
    // Transform flat arguments into Action + ParamsJson format for tools that expect it
    // This handles the case where MCP clients send {"action": "list", "blueprint_name": "..."} 
    // but the tool expects {"Action": "list", "ParamsJson": "{\"blueprint_name\":\"...\"}"}
    if (Arguments.Contains(TEXT("action")) || Arguments.Contains(TEXT("Action")))
    {
        FString ActionValue;
        if (Arguments.Contains(TEXT("action")))
        {
            ActionValue = Arguments.FindRef(TEXT("action"));
            Arguments.Remove(TEXT("action"));
        }
        else if (Arguments.Contains(TEXT("Action")))
        {
            ActionValue = Arguments.FindRef(TEXT("Action"));
            Arguments.Remove(TEXT("Action"));
        }
        
        // Check if this is NOT using the ParamsJson pattern already
        if (!Arguments.Contains(TEXT("ParamsJson")))
        {
            // Build ParamsJson from remaining arguments
            TSharedPtr<FJsonObject> ParamsObj = MakeShared<FJsonObject>();
            for (const auto& Pair : Arguments)
            {
                // Try to parse as JSON first (for nested objects/arrays)
                TSharedPtr<FJsonValue> JsonValue;
                TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Pair.Value);
                if (FJsonSerializer::Deserialize(Reader, JsonValue) && JsonValue.IsValid())
                {
                    ParamsObj->SetField(Pair.Key, JsonValue);
                }
                else
                {
                    ParamsObj->SetStringField(Pair.Key, Pair.Value);
                }
            }
            
            FString ParamsJsonStr;
            TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ParamsJsonStr);
            FJsonSerializer::Serialize(ParamsObj.ToSharedRef(), Writer);
            
            // Replace arguments with Action + ParamsJson
            Arguments.Empty();
            Arguments.Add(TEXT("Action"), ActionValue);
            Arguments.Add(TEXT("ParamsJson"), ParamsJsonStr);
        }
        else
        {
            // Already has ParamsJson, just ensure Action is capitalized
            Arguments.Add(TEXT("Action"), ActionValue);
        }
    }

    const FString ToolActionName = ExtractToolActionName(Arguments);
    if (!ToolActionName.IsEmpty())
    {
        FScopeLock Lock(&DiagnosticsLock);
        Diagnostics.LastToolsCallActionName = ToolActionName;
        for (FMCPToolCallRecord& Record : Diagnostics.RecentToolCalls)
        {
            if (Record.Sequence == ToolCallSequence)
            {
                Record.ActionName = ToolActionName;
                break;
            }
        }
    }
    
    // Check if tool exists (can be done on any thread)
    FToolRegistry& Registry = FToolRegistry::Get();
    const FToolMetadata* ToolMeta = Registry.FindTool(ToolName);
    if (!ToolMeta)
    {
        const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
        {
            FScopeLock Lock(&DiagnosticsLock);
            Diagnostics.bLastToolsCallIsError = true;
            Diagnostics.LastToolsCallErrorSummary = TEXT("Unknown tool");
        }
        FMCPStructuredToolResult StructuredResult;
        StructuredResult.ActionName = ToolActionName;
        StructuredResult.ErrorCode = TEXT("UNKNOWN_TOOL");
        CompleteToolRecord(StructuredResult, /*bIsError=*/ true, TEXT("Unknown tool"), DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
        RecordToolUsage(StructuredResult, /*bIsError=*/ true, TEXT("Unknown tool"), DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
        const FString ToolLabel = ToolActionName.IsEmpty()
            ? ToolName
            : FString::Printf(TEXT("%s:%s"), *ToolName, *ToolActionName);
        RecordDiagnosticsEvent(FString::Printf(TEXT("tools/call %s -> error (unknown tool)"), *ToolLabel));
        return BuildJsonRpcError(RequestId, -32602, FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
    }
    
    // Execute tool on game thread - REQUIRED because many UE operations are not thread-safe
    FString ToolResult;
    int32 ActorDelta = 0;
    int32 NewDirtyPackageCount = 0;
    TArray<FString> NewDirtyPackages;
    
    // Check if we're already on the game thread
    if (IsInGameThread())
    {
        // Execute directly - no need to defer
        UE_LOG(LogMCPServer, Log, TEXT("Executing tool %s directly on game thread"), *ToolName);
        const FMCPToolImpactSnapshot ImpactBefore = CaptureToolImpactSnapshot();
        ToolResult = FToolRegistry::Get().ExecuteTool(ToolName, Arguments);
        const FMCPToolImpactSnapshot ImpactAfter = CaptureToolImpactSnapshot();
        FMCPToolCallRecord ImpactDelta;
        ApplyImpactDelta(ImpactBefore, ImpactAfter, ImpactDelta);
        ActorDelta = ImpactDelta.ActorCountDelta;
        NewDirtyPackageCount = ImpactDelta.NewDirtyPackageCount;
        NewDirtyPackages = MoveTemp(ImpactDelta.NewDirtyPackages);
    }
    else
    {
        // We're on a background thread, need to marshal to game thread.
        // Use shared state so the lambda never touches dangling stack references
        // if the caller times out and its stack frame is destroyed.
        UE_LOG(LogMCPServer, Log, TEXT("Marshaling tool %s to game thread from socket thread"), *ToolName);
        
        struct FToolExecState
        {
            FString Result;
            int32 ActorDelta = 0;
            int32 NewDirtyPackageCount = 0;
            TArray<FString> NewDirtyPackages;
            TAtomic<bool> bStarted{false};
            TAtomic<bool> bTimedOut{false};
            FEvent* Event = nullptr;
            
            ~FToolExecState()
            {
                if (Event)
                {
                    FPlatformProcess::ReturnSynchEventToPool(Event);
                    Event = nullptr;
                }
            }
        };
        
        TSharedPtr<FToolExecState> State = MakeShared<FToolExecState>();
        State->Event = FPlatformProcess::GetSynchEventFromPool(false);
        
        AsyncTask(ENamedThreads::GameThread, [ToolName, Arguments, State]()
        {
            State->bStarted = true;
            
            if (State->bTimedOut)
            {
                UE_LOG(LogMCPServer, Warning, TEXT("Tool %s: caller already timed out before execution, skipping"), *ToolName);
                return;
            }
            
            UE_LOG(LogMCPServer, Log, TEXT("Tool %s execution starting on game thread"), *ToolName);
            const FMCPToolImpactSnapshot ImpactBefore = CaptureToolImpactSnapshot();
            State->Result = FToolRegistry::Get().ExecuteTool(ToolName, Arguments);
            const FMCPToolImpactSnapshot ImpactAfter = CaptureToolImpactSnapshot();
            FMCPToolCallRecord ImpactDelta;
            ApplyImpactDelta(ImpactBefore, ImpactAfter, ImpactDelta);
            State->ActorDelta = ImpactDelta.ActorCountDelta;
            State->NewDirtyPackageCount = ImpactDelta.NewDirtyPackageCount;
            State->NewDirtyPackages = MoveTemp(ImpactDelta.NewDirtyPackages);
            UE_LOG(LogMCPServer, Log, TEXT("Tool %s execution completed on game thread"), *ToolName);
            
            if (!State->bTimedOut && State->Event)
            {
                State->Event->Trigger();
            }
        });
        
        // Wait for completion with timeout (60 seconds for MCP tool execution)
        // Python scripts and complex operations may need more time
        const double TimeoutSeconds = static_cast<double>(FMath::Max(Config.ToolExecutionTimeoutMs, kMinToolExecutionTimeoutMs)) / 1000.0;
        const double StartWaitTime = FPlatformTime::Seconds();
        const double WaitSliceSeconds = 0.05; // Keep the socket thread responsive to shutdown

        bool bCompleted = false;
        while (true)
        {
            if (State->Event->Wait(FTimespan::FromSeconds(WaitSliceSeconds)))
            {
                bCompleted = true;
                break;
            }

            // Avoid deadlock: StopServer() is called on the game thread and waits for this socket thread.
            // If we're also waiting for the game thread to run the tool, both threads can block forever.
            if (bShouldStop)
            {
                State->bTimedOut = true;
                UE_LOG(LogMCPServer, Warning, TEXT("Tool %s aborted - MCP server is shutting down"), *ToolName);
                const FString ErrorText = TEXT("Server shutting down");
                const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
                {
                    FScopeLock Lock(&DiagnosticsLock);
                    Diagnostics.bLastToolsCallIsError = true;
                    Diagnostics.LastToolsCallErrorSummary = ErrorText;
                }
                FMCPStructuredToolResult StructuredResult;
                StructuredResult.ActionName = ToolActionName;
                StructuredResult.ErrorCode = TEXT("SERVER_SHUTDOWN");
                CompleteToolRecord(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordToolUsage(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordDiagnosticsEvent(FString::Printf(TEXT("tools/call %s -> aborted (shutdown)"), *ToolName));
                return BuildJsonRpcError(RequestId, -32000, TEXT("MCP server is shutting down."));
            }

            if ((FPlatformTime::Seconds() - StartWaitTime) >= TimeoutSeconds)
            {
                break;
            }
        }
        
        if (bCompleted)
        {
            ToolResult = MoveTemp(State->Result);
            ActorDelta = State->ActorDelta;
            NewDirtyPackageCount = State->NewDirtyPackageCount;
            NewDirtyPackages = MoveTemp(State->NewDirtyPackages);
        }
        else
        {
            // Mark timeout so the lambda won't touch the event after we clean up
            State->bTimedOut = true;
            
            if (!State->bStarted)
            {
                UE_LOG(LogMCPServer, Error, TEXT("Tool %s execution never started (game thread blocked?) - timed out after %.1fs"), 
                    *ToolName, TimeoutSeconds);
                const FString ErrorText = TEXT("Timed out (never started)");
                const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
                {
                    FScopeLock Lock(&DiagnosticsLock);
                    Diagnostics.bLastToolsCallIsError = true;
                    Diagnostics.LastToolsCallErrorSummary = ErrorText;
                }
                FMCPStructuredToolResult StructuredResult;
                StructuredResult.ActionName = ToolActionName;
                StructuredResult.ErrorCode = TEXT("TOOL_TIMEOUT");
                CompleteToolRecord(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordToolUsage(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordDiagnosticsEvent(FString::Printf(TEXT("tools/call %s -> timeout (never started)"), *ToolName));
                return BuildJsonRpcError(RequestId, -32000, 
                    TEXT("Tool execution timed out - game thread may be blocked. Try again or restart the editor."));
            }
            else
            {
                UE_LOG(LogMCPServer, Error, TEXT("Tool %s execution started but didn't complete - timed out after %.1fs"), 
                    *ToolName, TimeoutSeconds);
                const FString ErrorText = TEXT("Timed out (in progress)");
                const double DurationMs = (FPlatformTime::Seconds() - ToolStartTime) * 1000.0;
                {
                    FScopeLock Lock(&DiagnosticsLock);
                    Diagnostics.bLastToolsCallIsError = true;
                    Diagnostics.LastToolsCallErrorSummary = ErrorText;
                }
                FMCPStructuredToolResult StructuredResult;
                StructuredResult.ActionName = ToolActionName;
                StructuredResult.ErrorCode = TEXT("TOOL_TIMEOUT");
                CompleteToolRecord(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordToolUsage(StructuredResult, /*bIsError=*/ true, ErrorText, DurationMs, /*ActorDelta=*/ 0, /*NewDirtyPackageCount=*/ 0, /*NewDirtyPackages=*/ {});
                RecordDiagnosticsEvent(FString::Printf(TEXT("tools/call %s -> timeout (in progress)"), *ToolName));
                return BuildJsonRpcError(RequestId, -32000, 
                    TEXT("Tool execution timed out while running. The operation may still be in progress."));
            }
        }
    }
    
    double ToolEndTime = FPlatformTime::Seconds();
    const double DurationMs = (ToolEndTime - ToolStartTime) * 1000.0;
    UE_LOG(LogMCPServer, Log, TEXT("Tool %s completed in %.2fms"), *ToolName, DurationMs);
    
    // Build MCP result
    TArray<TSharedPtr<FJsonValue>> ContentArray;
    
    TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
    TextContent->SetStringField(TEXT("type"), TEXT("text"));
    TextContent->SetStringField(TEXT("text"), ToolResult);
    ContentArray.Add(MakeShared<FJsonValueObject>(TextContent));
    
    // Check if result indicates an error by parsing the JSON
    const FMCPStructuredToolResult StructuredResult = ParseStructuredToolResult(ToolResult);

    bool bIsError = false;
    FString ErrorSummary;
    if (StructuredResult.bParsedJson)
    {
        bIsError = (StructuredResult.bHasSuccessField && !StructuredResult.bSuccess) || !StructuredResult.ErrorMessage.IsEmpty();
        ErrorSummary = StructuredResult.ErrorMessage;
        if (bIsError && ErrorSummary.IsEmpty())
        {
            ErrorSummary = !StructuredResult.Summary.IsEmpty() ? StructuredResult.Summary : StructuredResult.ErrorCode;
        }
    }
    else
    {
        // If we can't parse JSON, fall back to string check for actual error content
        bIsError = ToolResult.Contains(TEXT("\"error\":")) &&
                   !ToolResult.Contains(TEXT("\"error\":\"\"")) &&
                   !ToolResult.Contains(TEXT("\"error\": \"\""));
    }
    
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("content"), ContentArray);
    Result->SetBoolField(TEXT("isError"), bIsError);

    {
        FScopeLock Lock(&DiagnosticsLock);
        Diagnostics.LastToolsCallActionName = StructuredResult.ActionName.IsEmpty() ? ToolActionName : StructuredResult.ActionName;
        Diagnostics.bLastToolsCallIsError = bIsError;
        Diagnostics.LastToolsCallErrorSummary = bIsError ? ErrorSummary.Left(200) : FString();
    }
    CompleteToolRecord(StructuredResult, bIsError, ErrorSummary, DurationMs, ActorDelta, NewDirtyPackageCount, NewDirtyPackages);
    RecordToolUsage(StructuredResult, bIsError, ErrorSummary, DurationMs, ActorDelta, NewDirtyPackageCount, NewDirtyPackages);

    const FString ActorDeltaText = ActorDelta >= 0
        ? FString::Printf(TEXT("+%d"), ActorDelta)
        : FString::Printf(TEXT("%d"), ActorDelta);

    const FString ToolLabel = StructuredResult.ActionName.IsEmpty()
        ? ToolName
        : FString::Printf(TEXT("%s:%s"), *ToolName, *StructuredResult.ActionName);

    RecordDiagnosticsEvent(FString::Printf(
        TEXT("tools/call %s -> %s (%.0fms, dirty+%d, actors%s)"),
        *ToolLabel,
        bIsError ? TEXT("error") : TEXT("ok"),
        DurationMs,
        NewDirtyPackageCount,
        *ActorDeltaText));
    
    return BuildJsonRpcResponse(RequestId, Result);
}

FString FMCPServer::HandlePing(const FString& RequestId)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    return BuildJsonRpcResponse(RequestId, Result);
}

// ============ JSON-RPC Helpers ============

FString FMCPServer::BuildJsonRpcResponse(const FString& RequestId, TSharedPtr<FJsonObject> Result)
{
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    
    if (!RequestId.IsEmpty())
    {
        // Try to preserve numeric ID
        int32 NumericId;
        if (LexTryParseString(NumericId, *RequestId))
        {
            Response->SetNumberField(TEXT("id"), NumericId);
        }
        else
        {
            Response->SetStringField(TEXT("id"), RequestId);
        }
    }
    else
    {
        Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
    }
    
    Response->SetObjectField(TEXT("result"), Result);
    
    FString Output;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
    
    return Output;
}

FString FMCPServer::BuildJsonRpcError(const FString& RequestId, int32 Code, const FString& Message)
{
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
    
    if (!RequestId.IsEmpty())
    {
        int32 NumericId;
        if (LexTryParseString(NumericId, *RequestId))
        {
            Response->SetNumberField(TEXT("id"), NumericId);
        }
        else
        {
            Response->SetStringField(TEXT("id"), RequestId);
        }
    }
    else
    {
        Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
    }
    
    TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
    Error->SetNumberField(TEXT("code"), Code);
    Error->SetStringField(TEXT("message"), Message);
    Response->SetObjectField(TEXT("error"), Error);
    
    FString Output;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
    
    return Output;
}

// ============ Security ============

bool FMCPServer::ValidateApiKey(const TMap<FString, FString>& Headers) const
{
    if (Config.ApiKey.IsEmpty())
    {
        return true; // No auth required
    }
    
    // Check Authorization header
    const FString* AuthHeader = Headers.Find(TEXT("authorization"));
    if (!AuthHeader)
    {
        return false;
    }
    
    // Support "Bearer <key>" format
    if (AuthHeader->StartsWith(TEXT("Bearer "), ESearchCase::IgnoreCase))
    {
        FString ProvidedKey = AuthHeader->Mid(7);
        return ProvidedKey == Config.ApiKey;
    }
    
    // Also support raw API key
    return *AuthHeader == Config.ApiKey;
}

FMCPServerDiagnosticsSnapshot FMCPServer::GetDiagnosticsSnapshot(int32 MaxRecentEvents) const
{
    FMCPServerDiagnosticsSnapshot Snapshot;
    Snapshot.bIsRunning = bIsRunning;
    Snapshot.Port = Config.Port;
    Snapshot.ServerUrl = GetServerUrl();

    {
        FScopeLock Lock(&SessionLock);
        Snapshot.ActiveSessions = ActiveSessions.Num();
    }
    {
        FScopeLock Lock(&SSELock);
        Snapshot.ActiveSSEConnections = SSEConnections.Num();
    }

    {
        FScopeLock Lock(&DiagnosticsLock);
        Snapshot.TotalConnectionsAccepted = Diagnostics.TotalConnectionsAccepted;
        Snapshot.LastConnectionAcceptedUtc = Diagnostics.LastConnectionAcceptedUtc;

        Snapshot.TotalRpcRequests = Diagnostics.TotalRpcRequests;
        Snapshot.LastRpcUtc = Diagnostics.LastRpcUtc;
        Snapshot.LastRpcMethod = Diagnostics.LastRpcMethod;
        Snapshot.LastRpcId = Diagnostics.LastRpcId;
        Snapshot.LastRpcSessionId = Diagnostics.LastRpcSessionId;

        Snapshot.LastToolsCallUtc = Diagnostics.LastToolsCallUtc;
        Snapshot.LastToolsCallToolName = Diagnostics.LastToolsCallToolName;
        Snapshot.LastToolsCallActionName = Diagnostics.LastToolsCallActionName;
        Snapshot.bLastToolsCallIsError = Diagnostics.bLastToolsCallIsError;
        Snapshot.LastToolsCallErrorSummary = Diagnostics.LastToolsCallErrorSummary;

        if (MaxRecentEvents <= 0 || Diagnostics.RecentEvents.Num() <= MaxRecentEvents)
        {
            Snapshot.RecentEvents = Diagnostics.RecentEvents;
        }
        else
        {
            const int32 StartIndex = Diagnostics.RecentEvents.Num() - MaxRecentEvents;
            Snapshot.RecentEvents.Reserve(MaxRecentEvents);
            Snapshot.RecentEvents.Append(Diagnostics.RecentEvents.GetData() + StartIndex, MaxRecentEvents);
        }

        Snapshot.RecentToolCalls = Diagnostics.RecentToolCalls;
    }

    return Snapshot;
}

void FMCPServer::RecordDiagnosticsEvent(const FString& EventLine)
{
    const FString Timestamp = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
    const FString FullLine = FString::Printf(TEXT("%s %s"), *Timestamp, *EventLine);

    FScopeLock Lock(&DiagnosticsLock);
    Diagnostics.RecentEvents.Add(FullLine);

    constexpr int32 MaxEvents = 200;
    const int32 Overflow = Diagnostics.RecentEvents.Num() - MaxEvents;
    if (Overflow > 0)
    {
        Diagnostics.RecentEvents.RemoveAt(0, Overflow);
    }
}

bool FMCPServer::ValidateOrigin(const TMap<FString, FString>& Headers) const
{
    // For localhost server, we're more permissive but still check Origin
    const FString* Origin = Headers.Find(TEXT("origin"));
    
    if (!Origin)
    {
        // No origin header - likely a non-browser client like curl or an IDE
        return true;
    }
    
    // Allow localhost origins
    if (Origin->Contains(TEXT("localhost")) || 
        Origin->Contains(TEXT("127.0.0.1")) ||
        Origin->StartsWith(TEXT("vscode-webview://")) ||
        Origin->StartsWith(TEXT("file://")))
    {
        return true;
    }
    
    UE_LOG(LogMCPServer, Warning, TEXT("Rejected request with Origin: %s"), **Origin);
    return false;
}

FString FMCPServer::GenerateSessionId() const
{
    return FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
}

// ============ Tool Access ============

TArray<FMCPTool> FMCPServer::GetInternalTools() const
{
    TArray<FMCPTool> Result;
    
    FToolRegistry& Registry = FToolRegistry::Get();
    if (!Registry.IsInitialized())
    {
        UE_LOG(LogMCPServer, Warning, TEXT("ToolRegistry not initialized"));
        return Result;
    }
    
    TArray<FToolMetadata> EnabledTools = Registry.GetEnabledTools();
    Result.Reserve(EnabledTools.Num());
    
    for (const FToolMetadata& Tool : EnabledTools)
    {
        // Skip internal-only tools - they are not exposed via MCP to external clients
        if (Tool.bInternalOnly)
        {
            UE_LOG(LogMCPServer, Verbose, TEXT("Skipping internal-only tool for MCP: %s"), *Tool.Name);
            continue;
        }
        
        FMCPTool MCPTool;
        MCPTool.Name = Tool.Name;
        MCPTool.Description = Tool.Description;
        MCPTool.ServerName = TEXT("VibeUE-Internal");
        
        // Build input schema
        TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
        InputSchema->SetStringField(TEXT("type"), TEXT("object"));
        
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> RequiredArray;
        
        for (const FToolParameter& Param : Tool.Parameters)
        {
            TSharedPtr<FJsonObject> ParamSchema = MakeShared<FJsonObject>();
            ParamSchema->SetStringField(TEXT("type"), Param.Type);
            ParamSchema->SetStringField(TEXT("description"), Param.Description);
            
            // For array types, add items schema (required by JSON Schema spec)
            if (Param.Type == TEXT("array"))
            {
                TSharedPtr<FJsonObject> ItemsSchema = MakeShared<FJsonObject>();
                ItemsSchema->SetStringField(TEXT("type"), TEXT("string")); // Default to string items
                ParamSchema->SetObjectField(TEXT("items"), ItemsSchema);
            }
            
            Properties->SetObjectField(Param.Name, ParamSchema);
            
            if (Param.bRequired)
            {
                RequiredArray.Add(MakeShared<FJsonValueString>(Param.Name));
            }
        }
        
        InputSchema->SetObjectField(TEXT("properties"), Properties);
        if (RequiredArray.Num() > 0)
        {
            InputSchema->SetArrayField(TEXT("required"), RequiredArray);
        }
        
        MCPTool.InputSchema = InputSchema;
        Result.Add(MCPTool);
    }
    
    return Result;
}

void FMCPServer::ProcessPendingRequests()
{
    // Process any requests that need to run on game thread
    // (Currently not used since tool execution happens synchronously)
    
    // Keep SSE connections healthy and detect disconnects.
    // Some IDE clients keep a persistent event-stream open even if we never send notifications.
    const double NowSeconds = FPlatformTime::Seconds();
    const double KeepAliveIntervalSeconds = 5.0;
    if ((NowSeconds - LastSSEKeepAliveSeconds) < KeepAliveIntervalSeconds)
    {
        return;
    }
    LastSSEKeepAliveSeconds = NowSeconds;

    const FString KeepAlive = TEXT(": keepalive\n\n");
    FTCHARToUTF8 Converter(*KeepAlive);

    FScopeLock Lock(&SSELock);
    for (const TSharedPtr<FMCPSSEConnection>& Conn : SSEConnections)
    {
        if (!Conn.IsValid() || !Conn->bIsActive || !Conn->ClientSocket)
        {
            continue;
        }

        int32 BytesSent = 0;
        const bool bSent = Conn->ClientSocket->Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), BytesSent);
        if (!bSent || BytesSent != Converter.Length())
        {
            Conn->bIsActive = false;
            Conn->ClientSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Conn->ClientSocket);
            Conn->ClientSocket = nullptr;
        }
    }

    const int32 OldCount = SSEConnections.Num();
    SSEConnections.RemoveAll([](const TSharedPtr<FMCPSSEConnection>& Conn) {
        return !Conn.IsValid() || !Conn->bIsActive || Conn->ClientSocket == nullptr;
    });
    if (SSEConnections.Num() != OldCount)
    {
        RecordDiagnosticsEvent(FString::Printf(TEXT("sse cleanup %d -> %d"), OldCount, SSEConnections.Num()));
    }
}

// ============ SSE Streaming Support ============

bool FMCPServer::HandleSSERequest(FSocket* ClientSocket, const TMap<FString, FString>& Headers)
{
    FString SessionId = Headers.FindRef(TEXT("mcp-session-id"));
    
    // Validate session exists if provided
    if (!SessionId.IsEmpty())
    {
        FScopeLock Lock(&SessionLock);
        if (!ActiveSessions.Contains(SessionId))
        {
            SendHttpResponse(ClientSocket, 404, TEXT("Not Found"), TEXT("text/plain"), TEXT("Session not found"));
            ClientSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ClientSocket);
            return false;
        }
    }
    
    // Check for Last-Event-ID for resumption
    FString LastEventIdStr = Headers.FindRef(TEXT("last-event-id"));
    int32 LastEventId = 0;
    if (!LastEventIdStr.IsEmpty())
    {
        LastEventId = FCString::Atoi(*LastEventIdStr);
        UE_LOG(LogMCPServer, Log, TEXT("SSE stream resuming from event ID: %d"), LastEventId);
    }
    
    // Send SSE response headers
    SendSSEResponse(ClientSocket, SessionId);
    
    // Create connection tracking
    TSharedPtr<FMCPSSEConnection> Connection = MakeShared<FMCPSSEConnection>();
    Connection->ClientSocket = ClientSocket;
    Connection->SessionId = SessionId;
    Connection->ConnectedAt = FDateTime::UtcNow();
    Connection->LastEventId = LastEventId;
    Connection->bIsActive = true;
    
    {
        FScopeLock Lock(&SSELock);
        SSEConnections.Add(Connection);
    }
    
    // Send initial empty event to prime reconnection (per MCP spec)
    int32 EventId = ++NextEventId;
    SendSSEEvent(ClientSocket, TEXT(""), EventId);
    
    // Send retry hint (1 second recommended by spec)
    FString RetryHint = TEXT("retry: 1000\n\n");
    FTCHARToUTF8 Converter(*RetryHint);
    int32 BytesSent = 0;
    ClientSocket->Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), BytesSent);
    
    UE_LOG(LogMCPServer, Log, TEXT("SSE stream opened for session: %s"), SessionId.IsEmpty() ? TEXT("<none>") : *SessionId);
    RecordDiagnosticsEvent(FString::Printf(TEXT("sse open %s"), SessionId.IsEmpty() ? TEXT("<none>") : *SessionId));
    
    // Note: We keep the connection open for server-initiated messages
    // The connection will be cleaned up when the client disconnects or on shutdown
    // For now, we don't have server-initiated messages, so we just keep it open
    // The client can close the connection when done
    
    return true;
}

void FMCPServer::SendSSEResponse(FSocket* Socket, const FString& SessionId)
{
    if (!Socket)
    {
        return;
    }
    
    FString Response = TEXT("HTTP/1.1 200 OK\r\n");
    Response += TEXT("Content-Type: text/event-stream\r\n");
    Response += TEXT("Cache-Control: no-cache\r\n");
    Response += TEXT("Connection: keep-alive\r\n");
    Response += TEXT("Access-Control-Allow-Origin: *\r\n");
    Response += TEXT("Access-Control-Allow-Headers: Content-Type, Authorization, Mcp-Session-Id, MCP-Protocol-Version, Accept, Last-Event-ID\r\n");
    
    if (!SessionId.IsEmpty())
    {
        Response += FString::Printf(TEXT("Mcp-Session-Id: %s\r\n"), *SessionId);
    }
    
    Response += TEXT("\r\n");
    
    FTCHARToUTF8 Converter(*Response);
    int32 BytesSent = 0;
    Socket->Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), BytesSent);
}

bool FMCPServer::SendSSEEvent(FSocket* ClientSocket, const FString& Data, int32 EventId)
{
    if (!ClientSocket)
    {
        return false;
    }
    
    FString Event;
    
    // Add event ID for resumability
    if (EventId >= 0)
    {
        Event += FString::Printf(TEXT("id: %d\n"), EventId);
    }
    
    // Data field - handle multi-line data
    if (Data.IsEmpty())
    {
        Event += TEXT("data: \n");
    }
    else
    {
        // SSE requires each line of data to be prefixed with "data: "
        TArray<FString> Lines;
        Data.ParseIntoArray(Lines, TEXT("\n"));
        for (const FString& Line : Lines)
        {
            Event += FString::Printf(TEXT("data: %s\n"), *Line);
        }
    }
    
    Event += TEXT("\n"); // Empty line to signal end of event
    
    FTCHARToUTF8 Converter(*Event);
    int32 BytesSent = 0;
    bool bSuccess = ClientSocket->Send(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length(), BytesSent);
    
    if (!bSuccess)
    {
        UE_LOG(LogMCPServer, Warning, TEXT("Failed to send SSE event"));
    }
    
    return bSuccess;
}

bool FMCPServer::ValidateProtocolVersion(const TMap<FString, FString>& Headers) const
{
    const FString* VersionHeader = Headers.Find(TEXT("mcp-protocol-version"));
    
    if (!VersionHeader)
    {
        // Per spec: if no header and no other way to identify version, assume 2025-03-26
        // We'll be permissive and allow missing header
        return true;
    }
    
    // Check if it's a version we support
    if (SUPPORTED_PROTOCOL_VERSIONS.Contains(*VersionHeader))
    {
        return true;
    }
    
    UE_LOG(LogMCPServer, Warning, TEXT("Unsupported MCP-Protocol-Version: %s"), **VersionHeader);
    return false;
}

bool FMCPServer::AcceptsSSE(const TMap<FString, FString>& Headers) const
{
    const FString* AcceptHeader = Headers.Find(TEXT("accept"));
    if (!AcceptHeader)
    {
        return false;
    }
    
    // Check if text/event-stream is in the Accept header
    return AcceptHeader->Contains(TEXT("text/event-stream"));
}

// ============ Config Persistence ============

void FMCPServer::LoadConfig()
{
    Config.bEnabled = GetEnabledFromConfig();
    Config.Port = GetPortFromConfig();
    Config.RequestReadTimeoutMs = GetRequestReadTimeoutMsFromConfig();
    Config.ToolExecutionTimeoutMs = GetToolExecutionTimeoutMsFromConfig();
    Config.ApiKey = GetApiKeyFromConfig();
}

void FMCPServer::SaveConfig()
{
    SaveEnabledToConfig(Config.bEnabled);
    SavePortToConfig(Config.Port);
    SaveRequestReadTimeoutMsToConfig(Config.RequestReadTimeoutMs);
    SaveToolExecutionTimeoutMsToConfig(Config.ToolExecutionTimeoutMs);
    SaveApiKeyToConfig(Config.ApiKey);
}

bool FMCPServer::GetEnabledFromConfig()
{
    bool bEnabled = true; // Default to enabled
    GConfig->GetBool(TEXT("VibeUE.MCPServer"), TEXT("Enabled"), bEnabled, GEditorPerProjectIni);
    return bEnabled;
}

void FMCPServer::SaveEnabledToConfig(bool bEnabled)
{
    GConfig->SetBool(TEXT("VibeUE.MCPServer"), TEXT("Enabled"), bEnabled, GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

int32 FMCPServer::GetPortFromConfig()
{
    int32 Port = kDefaultPort;
    GConfig->GetInt(TEXT("VibeUE.MCPServer"), TEXT("Port"), Port, GEditorPerProjectIni);
    TryGetCommandLineIntOverride(TEXT("VibeUEMCPPort="), Port);
    return ClampConfiguredValue(Port, kDefaultPort, kMinPort, kMaxPort);
}

void FMCPServer::SavePortToConfig(int32 Port)
{
    GConfig->SetInt(TEXT("VibeUE.MCPServer"), TEXT("Port"), ClampConfiguredValue(Port, kDefaultPort, kMinPort, kMaxPort), GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

FString FMCPServer::GetApiKeyFromConfig()
{
    FString ApiKey;
    GConfig->GetString(TEXT("VibeUE.MCPServer"), TEXT("BearerToken"), ApiKey, GEditorPerProjectIni);
    if (ApiKey.IsEmpty())
    {
        GConfig->GetString(TEXT("VibeUE.MCPServer"), TEXT("ApiKey"), ApiKey, GEditorPerProjectIni);
    }
    return ApiKey;
}

void FMCPServer::SaveApiKeyToConfig(const FString& ApiKey)
{
    GConfig->SetString(TEXT("VibeUE.MCPServer"), TEXT("BearerToken"), *ApiKey, GEditorPerProjectIni);
    GConfig->SetString(TEXT("VibeUE.MCPServer"), TEXT("ApiKey"), *ApiKey, GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

int32 FMCPServer::GetRequestReadTimeoutMsFromConfig()
{
    int32 TimeoutMs = kDefaultRequestReadTimeoutMs;
    GConfig->GetInt(TEXT("VibeUE.MCPServer"), TEXT("RequestReadTimeoutMs"), TimeoutMs, GEditorPerProjectIni);
    TryGetCommandLineIntOverride(TEXT("VibeUEMCPRequestReadTimeoutMs="), TimeoutMs);
    return ClampConfiguredValue(TimeoutMs, kDefaultRequestReadTimeoutMs, kMinRequestReadTimeoutMs, kMaxRequestReadTimeoutMs);
}

void FMCPServer::SaveRequestReadTimeoutMsToConfig(int32 TimeoutMs)
{
    GConfig->SetInt(
        TEXT("VibeUE.MCPServer"),
        TEXT("RequestReadTimeoutMs"),
        ClampConfiguredValue(TimeoutMs, kDefaultRequestReadTimeoutMs, kMinRequestReadTimeoutMs, kMaxRequestReadTimeoutMs),
        GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

int32 FMCPServer::GetToolExecutionTimeoutMsFromConfig()
{
    int32 TimeoutMs = kDefaultToolExecutionTimeoutMs;
    GConfig->GetInt(TEXT("VibeUE.MCPServer"), TEXT("ToolExecutionTimeoutMs"), TimeoutMs, GEditorPerProjectIni);
    TryGetCommandLineIntOverride(TEXT("VibeUEMCPToolExecutionTimeoutMs="), TimeoutMs);
    return ClampConfiguredValue(TimeoutMs, kDefaultToolExecutionTimeoutMs, kMinToolExecutionTimeoutMs, kMaxToolExecutionTimeoutMs);
}

void FMCPServer::SaveToolExecutionTimeoutMsToConfig(int32 TimeoutMs)
{
    GConfig->SetInt(
        TEXT("VibeUE.MCPServer"),
        TEXT("ToolExecutionTimeoutMs"),
        ClampConfiguredValue(TimeoutMs, kDefaultToolExecutionTimeoutMs, kMinToolExecutionTimeoutMs, kMaxToolExecutionTimeoutMs),
        GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}
