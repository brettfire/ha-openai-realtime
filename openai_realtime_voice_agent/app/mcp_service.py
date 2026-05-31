"""MCP service integration using Pipecat's MCPClient with StreamableHTTP."""
import logging
from typing import Optional
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client
from pipecat.services.mcp_service import MCPClient, StreamableHttpParameters

logger = logging.getLogger(__name__)


def _log_unwrapped(action: str, exc: BaseException) -> None:
    """Log an exception including any wrapped sub-exceptions.

    anyio TaskGroup / Python BaseExceptionGroup hide the real cause
    behind a generic "unhandled errors in a TaskGroup (N sub-exception)"
    string. Unwrap so we can actually see what broke.
    """
    if isinstance(exc, BaseExceptionGroup):
        logger.warning(
            "⚠️ Failed to %s (group of %d): %s",
            action,
            len(exc.exceptions),
            exc,
        )
        for i, sub in enumerate(exc.exceptions):
            logger.warning(
                "    sub-exception %d: %s: %s",
                i,
                type(sub).__name__,
                sub,
            )
    else:
        logger.warning(
            "⚠️ Failed to %s: %s: %s", action, type(exc).__name__, exc
        )


class HomeAssistantMCPService:
    """Home Assistant MCP service using Pipecat's MCPClient."""

    def __init__(self, url: str, access_token: str):
        """
        Initialize Home Assistant MCP service.

        Args:
            url: Home Assistant MCP Server URL (e.g., http://supervisor/core/api/mcp)
            access_token: Long-lived access token for Home Assistant
        """
        self.url = url
        self.access_token = access_token
        self.mcp_client: Optional[MCPClient] = None

    async def initialize(self) -> MCPClient:
        """Initialize and return the MCP client."""
        try:
            logger.info(f"🔗 Initializing Home Assistant MCP Client at {self.url}")

            # Create StreamableHTTP parameters with authentication
            server_params = StreamableHttpParameters(
                url=self.url,
                headers={
                    "Authorization": f"Bearer {self.access_token}"
                }
            )

            # Create MCP client
            self.mcp_client = MCPClient(server_params=server_params)

            logger.info("✅ Home Assistant MCP Client initialized")
            return self.mcp_client

        except Exception as e:
            logger.error(f"❌ Failed to initialize Home Assistant MCP Client: {e}", exc_info=True)
            raise

    def get_client(self) -> Optional[MCPClient]:
        """Get the MCP client instance."""
        return self.mcp_client

    async def fetch_assist_prompt_and_snapshot(
        self,
    ) -> tuple[Optional[str], Optional[str]]:
        """Fetch HA's Assist system prompt AND live-context snapshot via MCP.

        HA's mcp_server exposes three things — tools, prompts, and resources.
        Pipecat's MCPClient only fetches tools, so without this call the
        OpenAI Realtime session knows the verbs (HassTurnOn/Off,
        GetLiveContext, ...) but not the nouns. We pull both pieces in a
        single short-lived streamable-HTTP session:

        - prompts/get on the default prompt returns ``llm_api.api_prompt``
          — the rich system prompt regular Assist injects on every
          request (exposed entities, areas, capabilities).
        - resources/read on ``homeassistant://assist/context-snapshot``
          returns the same content as the ``GetLiveContext`` tool — a
          live snapshot of current entity states. Front-loading this
          spares the model a tool round-trip for "what's currently on?"
          style questions and gives it concrete state to ground in.

        Returns a (prompt, snapshot) tuple; either element may be None
        if the server doesn't offer that piece (e.g. no exposed entities
        means no GetLiveContext tool, which suppresses the snapshot
        resource).
        """
        if not self.url:
            return (None, None)
        headers = (
            {"Authorization": f"Bearer {self.access_token}"}
            if self.access_token
            else None
        )
        prompt_text: Optional[str] = None
        snapshot_text: Optional[str] = None
        snapshot_uri = "homeassistant://assist/context-snapshot"
        async with streamablehttp_client(url=self.url, headers=headers) as (
            read_stream,
            write_stream,
            _,
        ):
            async with ClientSession(read_stream, write_stream) as session:
                init_result = await session.initialize()
                caps = getattr(init_result, "capabilities", None)
                prompts_supported = (
                    caps is not None and getattr(caps, "prompts", None) is not None
                )
                resources_supported = (
                    caps is not None and getattr(caps, "resources", None) is not None
                )

                # Default prompt (entity catalog, etc.). HA 2026.x advertises
                # prompts capability; older / non-HA servers may not. Best-
                # effort; a failure here shouldn't suppress the snapshot.
                if prompts_supported:
                    try:
                        prompts_result = await session.list_prompts()
                        if prompts_result.prompts:
                            prompt_result = await session.get_prompt(
                                prompts_result.prompts[0].name
                            )
                            if prompt_result.messages:
                                prompt_text = getattr(
                                    prompt_result.messages[0].content,
                                    "text",
                                    None,
                                )
                    except BaseException as e:  # noqa: BLE001 — unwrap below
                        _log_unwrapped("fetch Assist prompt", e)

                # Live-context snapshot. Added to HA's mcp_server in
                # 2026.5 (PR #167396, merged 2026-04-07 — after the
                # 2026.4.x branch froze). On 2026.4.x the resources
                # capability isn't advertised, so the gate below quietly
                # skips the call instead of triggering a "Method not
                # found" warning. Pass the AnyUrl from list_resources
                # straight back to read_resource; the HA server compares
                # `str(uri) != SNAPSHOT_RESOURCE_URI` and pydantic-
                # normalized strings may not match the literal.
                if resources_supported:
                    try:
                        resources_result = await session.list_resources()
                        matching = next(
                            (
                                r
                                for r in resources_result.resources
                                if str(r.uri) == snapshot_uri
                            ),
                            None,
                        )
                        if matching is not None:
                            read_result = await session.read_resource(matching.uri)
                            if read_result.contents:
                                snapshot_text = getattr(
                                    read_result.contents[0], "text", None
                                )
                    except BaseException as e:  # noqa: BLE001 — unwrap below
                        _log_unwrapped("fetch live-context snapshot", e)
                else:
                    logger.info(
                        "ℹ️ MCP server does not advertise resources capability "
                        "— live-context snapshot disabled (requires HA 2026.5+)"
                    )

        return (prompt_text, snapshot_text)

    async def fetch_assist_prompt(self) -> Optional[str]:
        """Backward-compatible wrapper around fetch_assist_prompt_and_snapshot.

        Returns just the prompt text, dropping the snapshot. Kept so
        existing callers that only want the prompt still work.
        """
        prompt, _ = await self.fetch_assist_prompt_and_snapshot()
        return prompt






