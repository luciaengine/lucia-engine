namespace GameEngine.Scripting
{
    /// <summary>Defines generated chunk iteration for a partial struct's Execute method.
    /// Component parameters select entities; EntityCommands provides structural edits.</summary>
    public interface IEntitySystem
    {
        /// <summary>Execution order. Lower runs first. Default 0.</summary>
        int Order => 0;

        /// <summary>Whether this system is currently enabled. Can be toggled via
        /// <c>GameSystemRunner.SetSystemEnabled(name, enabled)</c>.</summary>
        bool Enabled => true;
    }
}
