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
            'cmake-target' = 'libdesktop-core'
            'test-label' = 'libdesktop'
            'debug-path' = 'build/debug/{libprefix}libdesktop-core{lib}'
            'release-path' = 'build/release/{libprefix}libdesktop-core{lib}'
            platforms = @('x64-windows')
        }
    )
}
