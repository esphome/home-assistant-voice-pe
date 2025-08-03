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
    const response = await elevenlabs.conversationalAi.agents.simulateConversation(agentId, {
        simulationSpecification: {
            simulatedUserConfig: {
                prompt: {
                    prompt: `Hey, Jarvis. Could you check my calendar for me for today?`,
                },
            },
            toolMockConfig: {
                "calendar_agent": {
                    defaultIsError: false,
                    defaultReturnValue: "Your calendar is clear for today."
                }
            }
        },
        extraEvaluationCriteria: [
            {
                id: 'calendar_agent',
                name: 'Calendar agent check',
                conversationGoalPrompt: 'The agent checked the calendar using the calendar_agent tool.',
            },
        ],
    });

    console.log(JSON.stringify(response, null, 4));

    t.assert.equal(response.analysis.callSuccessful, 'success');
})