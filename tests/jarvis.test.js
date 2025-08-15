import test from 'node:test';
import dotenv from 'dotenv';
import { ElevenLabsClient } from '@elevenlabs/elevenlabs-js';

dotenv.config();

test('test', async t => {
    const apiKey = process.env.ELEVENLABS_API_KEY;
    const elevenlabs = new ElevenLabsClient({
        apiKey: apiKey,
    });
    const agentId = process.env.ELEVENLABS_AGENT_ID;

    const response = await fetch(`https://api.elevenlabs.io/v1/convai/agents/${agentId}/simulate-conversation`, {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'xi-api-key': apiKey,
        },
        body: JSON.stringify({
            simulation_specification: {
                simulated_user_config: {
                    prompt: {
                        mcp_server_ids: ['Ti8dOqqWh2D7FPwXHiBF'],
                        llm: 'gemini-2.5-flash',
                        temperature: 0,
                        tool_ids: [],
                    }
                },
                partial_conversation_history: [
                    {
                        "role": "user",
                        "message": "Hello!  I'd like to check my calendar.",
                        "time_in_call_secs": 0,
                    },
                ]
            },
            new_turns_limit: 3
        })
    });

    const content = await response.json();

    console.log(JSON.stringify(content, null, 2));

    t.assert.equal(content.analysis.call_successful, 'success');
})