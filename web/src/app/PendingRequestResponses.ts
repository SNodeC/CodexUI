import type {PendingRequestPresentation} from "../presentation/PresentationModel.js";
import {isObject, member} from "../presentation/PresentationProtocol.js";

export interface PendingResponse {result?: unknown; error?: {code: number; message: string}}
const error = (message: string): PendingResponse => ({error: {code: -32601, message}});

export function negativePendingResponse(request: PendingRequestPresentation): PendingResponse {
    if (request.kind === "command-approval" || request.kind === "file-change-approval") return {result: {decision: "decline"}};
    if (request.kind === "mcp-elicitation") return {result: {action: "decline", content: null, _meta: null}};
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval")
        return {result: {decision: {denied: {rejection: "Denied by user"}}}};
    if (request.kind === "dynamic-tool-call") return {result: {contentItems: [{type: "inputText", text: "Request declined by user"}], success: false}};
    return error("Request declined by user");
}

export function positivePendingResponse(request: PendingRequestPresentation, input: unknown = {}): PendingResponse {
    if (request.kind === "command-approval" || request.kind === "file-change-approval") return {result: {decision: "accept"}};
    if (request.kind === "user-input") return {result: {answers: input}};
    if (request.kind === "mcp-elicitation") return {result: {action: "accept", content: isObject(input) ? input : null, _meta: null}};
    if (request.kind === "permissions-approval") return {result: {permissions: member(request.raw, "permissions", {}), scope: "turn"}};
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval") return {result: {decision: "approved"}};
    if (request.kind === "dynamic-tool-call") return {result: {contentItems: [{type: "inputText", text: "CodexUI does not provide this dynamic tool"}], success: false}};
    return error("CodexUI does not support this server request");
}
