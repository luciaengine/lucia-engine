# Scene collaboration preview

The relay supports a private room with live Scene View cursors, text chat, and
one-to-one WebRTC call signaling. Collaborators must open the same saved scene
path and viewport mode to see each other's cursor. A cursor represents the
pointer's position within that viewport. It does not represent a picked world
position.

Start the relay on loopback with a random invite token:

```sh
export LUCIA_COLLAB_ROOM=studio
export LUCIA_COLLAB_TOKEN="$(openssl rand -hex 32)"
export LUCIA_COLLAB_STATE=/absolute/path/to/private-room-state
node Tools/SceneCollaboration/relay.mjs
```

For other machines, serve the relay through a private HTTPS endpoint. Give the
invite token to room members. Use a separate relay and token for each project.
Keep the token out of project files. Run the Editor with these environment
variables to show collaboration cursors:

```sh
export LUCIA_COLLAB_RELAY=http://127.0.0.1:8746
export LUCIA_COLLAB_ROOM=studio
export LUCIA_COLLAB_NAME="Alice"
# Set LUCIA_COLLAB_TOKEN to the room's invite token.
```

Open the relay URL in the Web panel or a browser for chat. For voice and video,
use a browser with WebRTC and media permissions. Join the room, pick a
collaborator, and click **Start call**. The other person accepts the call. Each
person can turn their mic and camera on or off independently. The relay carries
signaling only. The current call uses direct peer connectivity and provides no
TURN service.

The relay can also store revisioned Inspector-property, material-color, and
script-text operations in its room journal. The Editor cursor client does not
publish or apply these operations yet. The relay never executes script text.
Scene cameras and selections are not shared by this preview.

Run the two-client relay checks with:

```sh
node --test Tools/SceneCollaboration/relay.test.mjs
```
