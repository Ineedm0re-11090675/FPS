// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Networking.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMCPServer, Log, All);

// Forward declarations
class FSocket;
class FTcpListener;
struct FMCPTool;

/**
 * MCP Host Configuration
 * Settings for exposing internal tools to external clients via Streamable HTTP
 * Note: Named FMCPHostConfig to avoid collision with FMCPServerConfig (client connection config)
 */
struct FMCPHostConfig
{
    /** Whether the MCP server is enabled */
    bool bEnabled = true;
    
    /** Port to listen on */
    int32 Port = 8101;
    
    /** Total time budget for reading an incoming HTTP request (headers + body) */
    int32 RequestReadTimeoutMs = 15000;

    /** Time budget for a tools/call request to finish on the game thread */
    int32 ToolExecutionTimeoutMs = 60000;

    /** Bearer token for authentication (empty = no auth required) */
    FString ApiKey;
};

/**
 * Represents a pending HTTP request to process on the game thread
 */
struct FMCPPendingRequest
{
    FSocket* ClientSocket = nullptr;
    FString Method;
    FString Path;
    TMap<FString, FString> Headers;
    FString Body;
    FString SessionId;
};

/**
 * Represents an active SSE stream connection
 */
struct FMCPSSEConnection
{
    FSocket* ClientSocket = nullptr;
    FString SessionId;
    FDateTime ConnectedAt;
    int32 LastEventId = 0;
    bool bIsActive = true;
};

/**
 * Lightweight diagnostic snapshot for in-editor status display.
 * Intended for debugging external MCP clients (Cursor/Codex/etc).
 */
struct FMCPToolCallRecord
{
    int64 Sequence = 0;
    FString RequestId;
    FString SessionId;
    FString ToolName;
    FString ActionName;

    FDateTime StartedUtc;
    FDateTime CompletedUtc;
    double DurationMs = 0.0;

    bool bInProgress = false;
    bool bIsError = false;
    FString Summary;
    FString ErrorCode;
    FString ErrorSummary;
    TArray<FString> ChangedItems;
    TArray<FString> VerificationHints;
    TArray<FString> Warnings;
    TArray<FString> AffectedAssets;

    // Best-effort impact summary (editor-only). Populated when we can measure.
    int32 ActorCountDelta = 0;
    int32 NewDirtyPackageCount = 0;
    TArray<FString> NewDirtyPackages;
};

struct FMCPServerDiagnosticsSnapshot
{
    bool bIsRunning = false;
    int32 Port = 0;
    FString ServerUrl;

    int32 ActiveSessions = 0;
    int32 ActiveSSEConnections = 0;

    int64 TotalConnectionsAccepted = 0;
    FDateTime LastConnectionAcceptedUtc;

    int64 TotalRpcRequests = 0;
    FDateTime LastRpcUtc;
    FString LastRpcMethod;
    FString LastRpcId;
    FString LastRpcSessionId;

    FDateTime LastToolsCallUtc;
    FString LastToolsCallToolName;
    FString LastToolsCallActionName;
    bool bLastToolsCallIsError = false;
    FString LastToolsCallErrorSummary;

    TArray<FString> RecentEvents;
    TArray<FMCPToolCallRecord> RecentToolCalls;
};

/**
 * MCP Server - Exposes internal tools to external MCP clients
 * 
 * Implements the Model Context Protocol (MCP) Streamable HTTP transport
 * allowing clients like VS Code, Cursor, and Claude Desktop to use
 * VibeUE's internal tools.
 * 
 * @see https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#streamable-http
 */
class VIBEUE_API FMCPServer : public FRunnable
{
public:
    FMCPServer();
    virtual ~FMCPServer();
    
    /** Get singleton instance */
    static FMCPServer& Get();
    
    /** Initialize the server (loads config, optionally starts if enabled) */
    void Initialize();
    
    /** Shutdown the server */
    void Shutdown();
    
    /** Start the HTTP server */
    bool Start();
    
    /** Stop the HTTP server */
    void StopServer();
    
    /** Check if server is running */
    bool IsRunning() const { return bIsRunning; }
    
    /** Get current configuration */
    const FMCPHostConfig& GetConfig() const { return Config; }
    
    /** Get the server URL for display */
    FString GetServerUrl() const;

    /** Get a snapshot of recent MCP activity for UI/debugging */
    FMCPServerDiagnosticsSnapshot GetDiagnosticsSnapshot(int32 MaxRecentEvents = 20) const;
    
    // ============ FRunnable Interface ============
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override { bShouldStop = true; }
    virtual void Exit() override;
    
    // ============ Config Persistence ============
    
    /** Get MCP Server enabled state from config */
    static bool GetEnabledFromConfig();
    
    /** Save MCP Server enabled state to config */
    static void SaveEnabledToConfig(bool bEnabled);
    
    /** Get MCP Server port from config */
    static int32 GetPortFromConfig();
    
    /** Save MCP Server port to config */
    static void SavePortToConfig(int32 Port);
    
    /** Get MCP Server API key from config */
    static FString GetApiKeyFromConfig();
    
    /** Save MCP Server API key to config */
    static void SaveApiKeyToConfig(const FString& ApiKey);

    /** Get MCP request read timeout from config */
    static int32 GetRequestReadTimeoutMsFromConfig();

    /** Save MCP request read timeout to config */
    static void SaveRequestReadTimeoutMsToConfig(int32 TimeoutMs);

    /** Get MCP tool execution timeout from config */
    static int32 GetToolExecutionTimeoutMsFromConfig();

    /** Save MCP tool execution timeout to config */
    static void SaveToolExecutionTimeoutMsToConfig(int32 TimeoutMs);
    
    /** Load all config into Config struct */
    void LoadConfig();
    
    /** Save all config from Config struct */
    void SaveConfig();
    
    /** Process pending requests on game thread (called by tick) */
    void ProcessPendingRequests();

private:
    void RecordDiagnosticsEvent(const FString& EventLine);

    /** Handle incoming connection */
    bool HandleConnection(FSocket* ClientSocket);
    
    /** Parse HTTP request from socket */
    bool ParseHttpRequest(FSocket* Socket, FString& OutMethod, FString& OutPath, 
                          TMap<FString, FString>& OutHeaders, FString& OutBody);
    
    /** Send HTTP response */
    void SendHttpResponse(FSocket* Socket, int32 StatusCode, const FString& StatusText,
                          const FString& ContentType, const FString& Body,
                          const TMap<FString, FString>& ExtraHeaders = TMap<FString, FString>());
    
    /** Handle MCP JSON-RPC request */
    FString HandleMCPRequest(const FString& JsonBody, FString& InOutSessionId, bool& bOutIsNotification);
    
    /** Handle SSE GET request - opens event stream */
    bool HandleSSERequest(FSocket* ClientSocket, const TMap<FString, FString>& Headers);
    
    /** Send SSE event to a client */
    bool SendSSEEvent(FSocket* ClientSocket, const FString& Data, int32 EventId = -1);
    
    /** Send SSE response with Content-Type: text/event-stream */
    void SendSSEResponse(FSocket* Socket, const FString& SessionId);
    
    /** Validate MCP-Protocol-Version header */
    bool ValidateProtocolVersion(const TMap<FString, FString>& Headers) const;
    
    /** Parse Accept header to check for SSE support */
    bool AcceptsSSE(const TMap<FString, FString>& Headers) const;
    
	    /** Handle individual JSON-RPC methods */
	    FString HandleInitialize(TSharedPtr<FJsonObject> Params, const FString& RequestId);
	    FString HandleToolsList(TSharedPtr<FJsonObject> Params, const FString& RequestId);
	    FString HandleToolsCall(TSharedPtr<FJsonObject> Params, const FString& RequestId, const FString& SessionId);
	    FString HandleResourcesList(TSharedPtr<FJsonObject> Params, const FString& RequestId);
	    FString HandleResourceTemplatesList(TSharedPtr<FJsonObject> Params, const FString& RequestId);
	    FString HandlePromptsList(TSharedPtr<FJsonObject> Params, const FString& RequestId);
	    FString HandlePing(const FString& RequestId);
    
    /** Build JSON-RPC response */
    FString BuildJsonRpcResponse(const FString& RequestId, TSharedPtr<FJsonObject> Result);
    FString BuildJsonRpcError(const FString& RequestId, int32 Code, const FString& Message);
    
    /** Validate API key if configured */
    bool ValidateApiKey(const TMap<FString, FString>& Headers) const;
    
    /** Validate Origin header for security */
    bool ValidateOrigin(const TMap<FString, FString>& Headers) const;
    
    /** Generate new session ID */
    FString GenerateSessionId() const;
    
    /** Get internal tools in MCP format */
    TArray<FMCPTool> GetInternalTools() const;
    
    /** Current configuration */
    FMCPHostConfig Config;

    /** In-memory diagnostics for debugging MCP connectivity */
    struct FMCPDiagnosticsState
    {
        int64 NextToolCallSequence = 1;

        int64 TotalConnectionsAccepted = 0;
        FDateTime LastConnectionAcceptedUtc;

        int64 TotalRpcRequests = 0;
        FDateTime LastRpcUtc;
        FString LastRpcMethod;
        FString LastRpcId;
        FString LastRpcSessionId;

        FDateTime LastToolsCallUtc;
        FString LastToolsCallToolName;
        FString LastToolsCallActionName;
        bool bLastToolsCallIsError = false;
        FString LastToolsCallErrorSummary;

        TArray<FString> RecentEvents;
        TArray<FMCPToolCallRecord> RecentToolCalls;
    };

    mutable FCriticalSection DiagnosticsLock;
    FMCPDiagnosticsState Diagnostics;
    double LastSSEKeepAliveSeconds = 0.0;
    
    /** Whether the server is currently running */
    bool bIsRunning = false;
    
    /** Signal to stop the server thread */
    bool bShouldStop = false;
    
    /** TCP listener socket */
    FSocket* ListenerSocket = nullptr;
    
    /** Server thread */
    FRunnableThread* ServerThread = nullptr;
    
    /** Active sessions */
    TMap<FString, FDateTime> ActiveSessions;
    
    /** Critical section for thread safety */
    mutable FCriticalSection SessionLock;
    
    /** Queue of requests to process on game thread */
    TQueue<FMCPPendingRequest> PendingRequests;
    
    /** Active SSE connections */
    TArray<TSharedPtr<FMCPSSEConnection>> SSEConnections;
    mutable FCriticalSection SSELock;
    
    /** Next SSE event ID for resumability */
    TAtomic<int32> NextEventId;
    
    /** Tick delegate handle */
    FTSTicker::FDelegateHandle TickDelegateHandle;
    
    /** Singleton instance */
    static TSharedPtr<FMCPServer> Instance;
};
