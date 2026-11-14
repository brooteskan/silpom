{
    "Source": "SilPOMFillWallHoles.azsl",
    "DepthStencilState": {
        "Depth": {
            "Enable": true,
            "CompareFunc": "GreaterEqual"
        }
    },
    "ProgramSettings": {
        "EntryPoints": [
            {
                "name": "MainVS",
                "type": "Vertex"
            },
            {
                "name": "MainPS",
                "type": "Fragment"
            }
        ]
    },
    "Supervariants": [
        {
            "Name": "NoMSAA",
            "AddBuildArguments": {
                "azslc": [
                    "--no-ms"
                ]
            }
        }
    ]
}
