{
    "Source": "SilPOMInspectColors.azsl",
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
    "DepthStencilState": {
        "Depth": {
            "Enable": false
        },
        "Stencil": {
            "Enable": false
        }
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
