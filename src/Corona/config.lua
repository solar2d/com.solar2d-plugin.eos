--
-- For more information on config.lua see the Project Configuration Guide at:
-- https://docs.coronalabs.com/guide/basics/configSettings
--

application =
{
	content =
	{
		width = 320,
		height = 480,
		scale = "zoomEven",
		-- scale = "letterbox",
		-- scale = "none",
	},
     eos = {
        encryptionKey = "xxxxxxxxxxxxx",
        clientId = "xxxxxxxx",
        clientSecret = "xxxxxxxx",
        productId = "xxxxxxxx",
        sandboxId = "xxxxxxxx",
        deploymentId = "xxxxxxxx",
        productName = "Test App",
        productVersion = "1.0",
    },
}
