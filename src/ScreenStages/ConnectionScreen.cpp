// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2016 Hotride

#include "ConnectionScreen.h"
#include <algorithm>
#include "../CrossUO.h"
#include "../CharacterList.h"
#include "../Managers/ScreenEffectManager.h"
#include "../Network/Packets.h"
#include "../Managers/ClilocManager.h"
#include "../Globals.h" // g_Language

CConnectionScreen g_ConnectionScreen;

CConnectionScreen::CConnectionScreen()
    : CBaseScreen(m_ConnectionGump)
{
}

CConnectionScreen::~CConnectionScreen()
{
}

void CConnectionScreen::Init()
{
    CBaseScreen::Init();
    m_Text = "";
    m_ConnectionFailed = false;
    m_Connected = false;
    m_Completed = false;
    m_ErrorCode = 0;
    m_Type = CST_LOGIN;

    g_ScreenEffectManager.UseSunrise();
    SmoothScreenAction = 0;

    m_Gump.PrepareTextures();
    m_Gump.WantUpdateContent = true;
}

astr_t CConnectionScreen::LoginMessage() const
{
    if (!m_Text.empty())
    {
        return m_Text;
    }
    auto cliloc = g_ClilocManager.Cliloc(g_Language);
    if (!m_ConnectionFailed)
    {
        return m_Connected ? cliloc->GetA(3000003, false, "Verifying Account...") :
                             cliloc->GetA(3000002, false, "Connecting...");
    }
    switch (m_ErrorCode)
    {
        case 0:
            return cliloc->GetA(3000007, false, "Incorrect name/password.");
        case 1:
            return cliloc->GetA(3000034, false, "Someone is already using this account.");
        case 2:
            return cliloc->GetA(3000035, false, "Your account has been blocked.");
        case 3:
            return cliloc->GetA(3000036, false, "Your account credentials are invalid.");
        case 4:
            return "Communication problem.";
        case 5:
            return "The IGR concurrency limit has been met.";
        case 6:
            return "The IGR time limit has been met.";
        case 7:
            return "General IGR authentication failure.";
        default:
            return cliloc->GetA(
                3000037, false, "Couldn't connect to Ultima Online.  Please try again in a few moments.");
    }
}

astr_t CConnectionScreen::Message() const
{
    switch (m_Type)
    {
        case CST_CHARACTER_LIST:
        {
            if (!m_ConnectionFailed)
            {
                return "Permanently delete " + g_CharacterList.GetSelectedName() + "?";
            }
            const astr_t text[6] = { g_ClilocManager.Cliloc(g_Language) ->GetA(3000018, false, "That character password is invalid."), g_ClilocManager.Cliloc(g_Language) ->GetA(3000019, false, "That character does not exist."), g_ClilocManager.Cliloc(g_Language) ->GetA(3000020, false, "That character is being played right now."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000021, false, "That character is not old enough to delete. The character must be 7 days old before it can be deleted."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000022, false, "That character is currently queued for backup and cannot be deleted."), g_ClilocManager.Cliloc(g_Language) ->GetA(3000023, false, "Couldn't carry out your request.") };
        return text[std::min(std::max(m_ErrorCode, 0), 6 - 1)];
        }
        case CST_GAME:
        {
            if (!m_ConnectionFailed)
            {
                return g_ClilocManager.Cliloc(g_Language)->GetA(3000001, false, "Entering Britannia...");
            }
            const astr_t text[3] = { g_ClilocManager.Cliloc(g_Language) ->GetA( 3000033, false, "Either the Account Name or Password you provided were incorrect. If this is a new account your account may not be active yet. Please try again shortly."), g_ClilocManager.Cliloc(g_Language) ->GetA(3000612, false, "Your character name is too short."), "No character to login with." };
        return text[std::min(std::max(m_ErrorCode, 0), 3 - 1)];
        }
        case CST_GAME_LOGIN:
        {
            const astr_t text[10] = { g_ClilocManager.Cliloc(g_Language)->GetA(3000007, false, "Incorrect password."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000009, false, "This character does not exist anymore.  You will have to recreate it."), g_ClilocManager.Cliloc(g_Language) ->GetA(3000006, false, "This character already exists.\nPlaying..."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000016, false, "The client could not attach to the game server. It must have been taken down, please wait a few minutes and try again."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000017, false, "The client could not attach to the game server. It must have been taken down, please wait a few minutes and try again."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000012, false, "Another character from this account is currently online in this world.  You must either log in as that character or wait for it to time out."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000013, false, "An error has occurred in the synchronization between the login servers and this world.  Please close your client and try again."), g_ClilocManager.Cliloc(g_Language) ->GetA( 3000005, false, "You have been idle for too long.  If you do not do anything in the next minute, you will be logged out."), "", "" };
        return text[std::min(std::max(m_ErrorCode, 0), 10 - 1)];
        }
        case CST_SELECT_PROFESSOIN:
            return "You must have three unique skills choosen!";
        case CST_CONLOST:
            return "Connection lost";
        default:
            return LoginMessage();
    }
}

void CConnectionScreen::SetConnectionFailed(bool val)
{
    m_ConnectionFailed = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetConnected(bool val)
{
    m_Connected = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetCompleted(bool val)
{
    m_Completed = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetErrorCode(int val)
{
    m_ErrorCode = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetType(CONNECTION_SCREEN_TYPE val)
{
    m_Type = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::SetTextA(const astr_t &val)
{
    m_Text = val;
    m_Gump.WantUpdateContent = true;
}

void CConnectionScreen::ProcessSmoothAction(uint8_t action)
{
    if (action == 0xFF)
    {
        action = SmoothScreenAction;
    }

    if (action == ID_SMOOTH_CS_GO_SCREEN_MAIN)
    {
        g_Game.InitScreen(GS_MAIN);
    }
    else if (action == ID_SMOOTH_CS_GO_SCREEN_CHARACTER)
    {
        g_Game.InitScreen(GS_CHARACTER);
    }
    else if (action == ID_SMOOTH_CS_GO_SCREEN_PROFESSION)
    {
        g_Game.InitScreen(GS_PROFESSION_SELECT);
    }
    else if (action == ID_SMOOTH_CS_SEND_DELETE)
    {
        CPacketDeleteCharacter(g_CharacterList.Selected).Send();
    }
}

void CConnectionScreen::OnKeyDown(const KeyEvent &ev)
{
    const auto key = EvKey(ev);
    switch (key)
    {
        case KEY_RETURN:
        case KEY_RETURN2:
        {
            if (m_Type == CST_CHARACTER_LIST)
            {
                CreateSmoothAction(ID_SMOOTH_CS_SEND_DELETE);
            }
            else if (m_Type == CST_SELECT_PROFESSOIN)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_PROFESSION);
            }
            else if (m_Type == CST_GAME)
            {
                if (m_ErrorCode > 0)
                {
                    CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
                }
            }
            else if (m_Type == CST_CONLOST || m_ConnectionFailed)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_MAIN);
            }

            break;
        }
        case KEY_ESCAPE:
        {
            if (m_Type == CST_CHARACTER_LIST)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
            }
            else if (m_Type == CST_SELECT_PROFESSOIN)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_PROFESSION);
            }
            else if (m_Type == CST_GAME)
            {
                if (m_ErrorCode > 0)
                {
                    CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_CHARACTER);
                }
            }
            else if (m_Type == CST_CONLOST || m_ConnectionFailed)
            {
                CreateSmoothAction(ID_SMOOTH_CS_GO_SCREEN_MAIN);
            }

            break;
        }
        default:
            break;
    }
}
