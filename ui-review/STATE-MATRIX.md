# CodexUI Current Interaction State Matrix

| Surface | State | Current behavior |
| --- | --- | --- |
| Connection | Disconnected | Conversation remains inspectable; mutation controls reflect unavailable controller transport. |
| Connection | Connected observer | Read operations remain available; mutations require explicit controller ownership. |
| Connection | Connected controller | Thread, turn, and request mutations are enabled. |
| Thread list | Background activity | Status changes without changing the user's selection. |
| Thread list | Selected thread removed | Selection clears and the conversation returns to its empty state. |
| New Thread | Draft | Dialog values and prompt remain local until the first admission starts creation. |
| New Thread | Creation pending | Admitted prompts appear as animated pending cards and remain bound to the draft. |
| Prompt | Locally admitted | Composer clears immediately; a gray animated card appears in the destination thread. |
| Prompt | Additional prompt admitted | Composer remains enabled; the card is queued behind the in-flight prompt for that thread. |
| Prompt | Acknowledged | Animation stops and the pending card reconciles with the authoritative user item. |
| Prompt | Failed | Animation stops and the card remains with an explicit error state. |
| Navigation | Switch away from pending prompt | Pending card and queue remain associated with their stable thread ID. |
| Navigation | Return before acknowledgment | The same animated pending card is displayed. |
| Navigation | Return to materialized running thread | Retained Plan, Agents, Changes, and other per-thread presentation reappear without an automatic destructive read. |
| Conversation | At bottom | New cards and stream updates smoothly follow the bottom with a short retargetable animation. |
| Conversation | User scrolls during smooth follow | The animation stops immediately and automatic following pauses. |
| Conversation | User scrolled upward | Automatic following pauses; a visible-card/pixel-offset anchor preserves the reading position through appends, reflow, and reconstruction. |
| Conversation | User returns to bottom | Automatic following resumes. |
| Composer | Short prompt | One-line compact height. |
| Composer | Multiline prompt | Editor overlays the unchanged message viewport; matching trailing scroll space is added without moving existing messages. |
| Composer | User reaches extended bottom | The final card sits above the composer with the normal gap and bottom-follow resumes. |
| Composer | Prompt shrinks | Trailing space is removed; Qt may clamp the scroll position to the reduced range. |
| Composer | Shrink clamps to conversation bottom | Bottom-follow is reactivated for subsequent incoming content. |
| Composer | Maximum prompt height | Editor stops growing and scrolls internally. |
| Command execution | No visible output | No output box is shown. |
| Shell output | Fits below 220 px | Box grows to content without a minimum blank area. |
| Shell output | Exceeds 220 px at bottom | Scrollbar appears and appended output follows the bottom. |
| Shell output | User scrolled upward | Output following pauses until its scrollbar returns to the bottom. |
| Center chrome | Wheel or touchpad input | Message view scrolls unless a nested scrollable control owns the event. |
| Info / State | Content exceeds viewport | Common styled vertical scrollbar appears as needed. |
| Info / Protocol | Content exceeds viewport | Styled log scrollbar appears; statistics remain below the expanding log. |
| Pending request | Unresolved | Thread and global attention surfaces identify required user action. |
| Pending request | Resolved | Actionable request disappears exactly once for its stable request identity. |
