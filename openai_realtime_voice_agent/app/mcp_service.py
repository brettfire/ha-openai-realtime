"""MCP service integration using Pipecat's MCPClient with StreamableHTTP."""
import logging
from typing import Optional
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client
from pipecat.services.mcp_service import MCPClient, StreamableHttpParameters

logger = logging.getLogger(__name__)


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

    async def fetch_assist_prompt(self) -> Optional[str]:
        """Fetch Home Assistant's default Assist system prompt via MCP.

        HA's mcp_server exposes a single prompt (named after the active LLM
        API, typically "Assist") whose content is ``llm_api.api_prompt`` —
        the rich system prompt regular Assist injects on every request,
        listing exposed entities, areas, and current states. Pipecat's
        MCPClient only fetches tools (and on 0.0.97 it doesn't expose
        the underlying ClientSession), so we open our own short-lived
        streamable-HTTP session for this one extra call.
        Returns the prompt text, or None if no prompt is offered.
        """
        if not self.url:
            return None
        headers = (
            {"Authorization": f"Bearer {self.access_token}"}
            if self.access_token
            else None
        )
        async with streamablehttp_client(url=self.url, headers=headers) as (
            read_stream,
            write_stream,
            _,
        ):
            async with ClientSession(read_stream, write_stream) as session:
                await session.initialize()
                prompts_result = await session.list_prompts()
                if not prompts_result.prompts:
                    return None
                prompt_name = prompts_result.prompts[0].name
                prompt_result = await session.get_prompt(prompt_name)
                if not prompt_result.messages:
                    return None
                content = prompt_result.messages[0].content
                return getattr(content, "text", None)






