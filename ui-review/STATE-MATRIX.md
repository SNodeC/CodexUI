# CodexUI Current Interaction State Matrix

| Surface | State | Current behavior |
| --- | --- | --- |
| Connection | Disconnected | Conversation remains inspectable; mutation controls reflect unavailable controller transport. |
| Connection | Connected observer | Read operations remain available; mutations require explicit controller ownership. |
| Connection | Connected controller | Thread, turn, and request mutations are enabled. |
| Connection | Authored transport selection rejected or queue-full | No automatic retry occurs; reopening Transport is prefilled with the retained selection. |
| Thread list | Background activity | Status changes without changing the user's selection. |
| Thread list | Selected thread removed | Selection clears and the conversation returns to its empty state. |
| New Thread | Draft | Dialog values and prompt remain local; one selected orange animated row appears immediately without creating an app-server thread. |
| New Thread | Thread created, first prompt pending | The same row is rekeyed to the authoritative ID and remains animated through the first `turn/start` callback. |
| New Thread | First prompt acknowledged | The same row switches to canonical styling without duplication or replacement. |
| New Thread | Creation or first prompt failed | Animation stops and the retained row adopts explicit failure styling. |
| Prompt | Locally admitted | Composer clears immediately; a muted-blue card appears in the destination thread, and its sweeping highlight starts only after one second without acknowledgment. |
| Prompt | Additional prompt admitted | Composer remains enabled; the card is queued behind the in-flight prompt for that thread. |
| Prompt | Authoritative item arrives before result | Exact `clientUserMessageId` correlation may bind the item, but the card remains pending until its operation callback. |
| Prompt | Acknowledged | The matching `turn/start` or `turn/steer` callback immediately ends pending feedback; the authoritative item inherits the card's stable visual key. |
| Prompt | Failed | Animation stops and the card remains with an explicit error state. |
| Prompt | Disconnect after admission | The authored prompt remains visible in explicit failed/uncertain recovery state; bridge-open never resends it automatically. |
| Navigation | Switch away from pending prompt | Pending card and queue remain associated with their stable thread ID. |
| Navigation | Return before acknowledgment | The same animated pending card is displayed. |
| Navigation | Return to materialized running thread | Plan, Agents, Changes, and other per-thread widget state reappears without an automatic destructive read. |
| Thread | First selection in a connection generation | One full read hydrates the thread's current graph nodes before prompt dispatch; Reload is the explicit forced read. |
| Thread | Hydration failed | Submission leaves the composer draft intact and performs no dispatch; Reload must succeed before admission. |
| Thread | Provider reports `notLoaded` | Resume completes before the queued prompt is dispatched. |
| Thread | Prompt reports thread not found | The dispatch fails without an automatic retry; its authored input remains available for explicit recovery. |
| Thread | Deleted or provider generation reset during prompt work | The local prompt is detached from the invalid thread and retained with exact admitted text and attachment links in definite-failure or uncertain recovery state. |
| Conversation | At bottom | New cards and stream updates smoothly follow the bottom with a short retargetable animation. |
| Conversation | User scrolls during smooth follow | The animation stops immediately and automatic following pauses. |
| Conversation | User scrolled upward | Automatic following pauses; a visible-card/pixel-offset anchor preserves the reading position through appends, reflow, and reconstruction. |
| Conversation | Nonvisual protocol update | Visible node values are unchanged, so no card, geometry, or scroll mutation occurs. |
| Conversation | Paused while history grows | The effective history window grows with appended cards so the visible anchor is not evicted. |
| Conversation | User returns to bottom | Automatic following resumes. |
| Composer | Short prompt | One-line compact height. |
| Composer | Multiline prompt | Editor overlays the unchanged message viewport; matching trailing scroll space is added without moving existing messages. |
| Composer | User reaches extended bottom | The final card reaches the overlay boundary; canonical 8 px spacing surrounds the moving divider and bottom-follow resumes. |
| Composer | Prompt shrinks | Trailing space is removed; Qt may clamp the scroll position to the reduced range. |
| Composer | Shrink clamps to conversation bottom | Bottom-follow is reactivated for subsequent incoming content. |
| Composer | Maximum prompt height | Editor stops growing and scrolls internally. |
| Command execution | No visible output | No output box is shown. |
| Command execution output | Fits below 220 px | Box grows to content without a minimum blank area. |
| Command execution output | Exceeds 220 px at bottom | Scrollbar appears and appended output follows the bottom. |
| Command execution output | User scrolled upward | Output following pauses until its scrollbar returns to the bottom. |
| Command text or output | Gesture reaches its scroll boundary | The nested view retains that gesture; a fresh outward gesture at the boundary scrolls the message view. |
| Center chrome | Wheel or touchpad input | Message view scrolls unless a nested command view owns the current gesture. |
| Info / State | Content exceeds viewport | Common styled vertical scrollbar appears as needed. |
| Info / Protocol | Content exceeds viewport | Styled diagnostic scrollbar appears; statistics remain below the expanding viewer. |
| Pending request | Unresolved | Thread and global attention surfaces identify required user action. |
| Pending request | Authored response rejected after admission | No automatic retry occurs; the request retains the authored response and reopens Review with that input. |
| Pending request | Resolved | Actionable request disappears exactly once for its stable request identity. |
