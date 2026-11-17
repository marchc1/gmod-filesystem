#include "gmad/AddonWhiteList.h"

// Bootil's Wildcard without copying both strings
bool globber( const char *wild, const char *string );

// The game's own list, the one in gmad/AddonWhiteList.h is gmad's upload check
static const char *const g_GameWhiteList[] =
{
	"lua/*.lua",
	"scenes/*.vcd",
	"particles/*.pcf",
	"resource/fonts/*.ttf",
	"scripts/vehicles/*.txt",
	"resource/localization/*/*.properties",
	"maps/*.bsp",
	"maps/*.lmp",
	"maps/*.nav",
	"maps/*.ain",
	"maps/*.png",
	"sound/*.wav",
	"sound/*.mp3",
	"sound/*.ogg",
	"materials/*.vmt",
	"materials/*.vtf",
	"materials/*.png",
	"materials/*.jpg",
	"materials/*.jpeg",
	"materials/colorcorrection/*.raw",
	"models/*.mdl",
	"models/*.vtx",
	"models/*.phy",
	"models/*.ani",
	"models/*.vvd",
	"gamemodes/*/*.txt",
	"gamemodes/*/*.fgd",
	"gamemodes/*/content/maps/*_ttt.txt",
	"gamemodes/*/content/data/*.txt",
	"gamemodes/*/logo.png",
	"gamemodes/*/icon24.png",
	"gamemodes/*/gamemode/*.lua",
	"gamemodes/*/entities/effects/*.lua",
	"gamemodes/*/entities/weapons/*.lua",
	"gamemodes/*/entities/entities/*.lua",
	"gamemodes/*/backgrounds/*.png",
	"gamemodes/*/backgrounds/*.jpg",
	"gamemodes/*/backgrounds/*.jpeg",
	"gamemodes/*/content/models/*.mdl",
	"gamemodes/*/content/models/*.vtx",
	"gamemodes/*/content/models/*.phy",
	"gamemodes/*/content/models/*.ani",
	"gamemodes/*/content/models/*.vvd",
	"gamemodes/*/content/materials/*.vmt",
	"gamemodes/*/content/materials/*.vtf",
	"gamemodes/*/content/materials/*.png",
	"gamemodes/*/content/materials/*.jpg",
	"gamemodes/*/content/materials/*.jpeg",
	"gamemodes/*/content/materials/colorcorrection/*.raw",
	"gamemodes/*/content/scenes/*.vcd",
	"gamemodes/*/content/particles/*.pcf",
	"gamemodes/*/content/resource/fonts/*.ttf",
	"gamemodes/*/content/scripts/vehicles/*.txt",
	"gamemodes/*/content/resource/localization/*/*.properties",
	"gamemodes/*/content/maps/*.bsp",
	"gamemodes/*/content/maps/*.nav",
	"gamemodes/*/content/maps/*.ain",
	"gamemodes/*/content/maps/thumb/*.png",
	"gamemodes/*/content/sound/*.wav",
	"gamemodes/*/content/sound/*.mp3",
	"gamemodes/*/content/sound/*.ogg",
	"data_static/*.txt",
	"data_static/*.dat",
	"data_static/*.json",
	"data_static/*.xml",
	"data_static/*.csv",
	"shaders/fxc/*.vcs",
	nullptr
};

std::string Addon::WhiteList::FilenameErrors( const std::string &strFileName, uint64_t nFileSize )
{
    for ( unsigned char c : strFileName )
    {
        if ( c <= 31 )
            return "Filename contains invalid character: Code point " + std::to_string( c );

        static constexpr const char *badChars = "\"*:<>?|";
        if ( strchr( badChars, c ) )
            return "Filename contains invalid character '" + std::string( 1, c ) + "': Code point " + std::to_string( c );
    }

    for ( int i = 0; g_GameWhiteList[i] != nullptr; ++i )
    {
        const char *pszPattern = g_GameWhiteList[i];

        // No pattern starts with a wildcard, the first character has to match
        if ( pszPattern[0] != strFileName.c_str()[0] )
            continue;

        bool bMatched = globber( pszPattern, strFileName.c_str() );
        if ( bMatched && ( strcmp( pszPattern, "gamemodes/*/*.txt" ) == 0 || strcmp( pszPattern, "gamemodes/*/*.fgd" ) == 0 ) )
        {
            int slashCount = 0;
            for ( char c : strFileName )
            {
                if ( c == '/' )
                    ++slashCount;
            }

            if ( slashCount >= 3 )
                bMatched = false;
        }

        if ( !bMatched )
            continue;

        if ( strcmp( pszPattern, "gamemodes/*/content/data/*.txt" ) == 0 ||
             strcmp( pszPattern, "gamemodes/*/content/maps/*_ttt.txt" ) == 0 )
        {
            if ( nFileSize > 0x65B97160ULL )
                continue;
        }

        return "";
    }

    return "File is not allowed by whitelist";
}
