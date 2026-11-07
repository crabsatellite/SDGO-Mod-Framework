# Login unit showcase

Choose the units shown by the original login page in **SDGO Mod Manager → Login unit showcase**.

- **Grades:** choose one or more exact grades. The default is **SS, SR and SU**; ordinary S is a separate choice.
- **All units:** use every displayable initial-form unit registered in this client.
- **Recently registered:** choose the most recently registered units and set how many to include. Registration order comes from the client catalog, rather than the numerical unit ID or a file's modification time.
- **Selected units:** search the installed catalog by name and choose a list of units.

Within one game process, the selected pool is shuffled, avoiding repeated units until it has been traversed. The login page keeps its original presentation. Choices affect the login scene, and configuration changes take effect on the next game start. An empty or unavailable pool retains the original login selection.

Oversized replacement units are reduced when needed to fit the original login presentation; smaller units are not enlarged. The original camera and presentation remain in use, so outer weapons or wings may still touch the edge of the native close-up. Complete client updates retain the player's showcase mode and choices.

Imported units become candidates when their paired client content is installed and active. This setting does not grant units or change their battle properties. Remove unavailable units from a selected list, or choose another mode, when changing content packs.

The startup crash fix remains the owner of the native loading hook. If optional model fitting is missing or fails verification, the expanded showcase stays off while the original missing-unit protection continues working. Update the complete framework package to restore the showcase.
