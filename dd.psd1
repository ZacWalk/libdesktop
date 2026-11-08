@{
    schema = 1
    project = @{
        name = 'libdesktop'
        type = 'library'
        'default-target' = 'lib'
    }
    dependencies = @{ owner = 'dd' }
    build = @{
        'x64-windows' = @{
            debug = 'debug'
            release = 'release'
        }
    }
    targets = @(
        @{
            id = 'lib'
            kind = 'library'
            'cmake-target' = 'libdesktop-ui'
            'test-label' = 'libdesktop-ui'
            'debug-path' = 'build/debug/{libprefix}libdesktop-ui{lib}'
            'release-path' = 'build/release/{libprefix}libdesktop-ui{lib}'
            platforms = @('x64-windows')
        }
    )
}
